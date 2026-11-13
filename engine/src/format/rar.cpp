// RAR 5 archive reader, written for DigZip from the public description of the RAR 5 format.
// Decompression is in codec/rar5_unpack.cpp.

#include "codec/blake2sp.hpp"
#include "codec/encryption.hpp"
#include "codec/rar5.hpp"
#include "crypto/crypto.hpp"
#include "format/common.hpp"
#include "format/reader.hpp"
#include "text.hpp"

#include <digzip/error.hpp>

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <map>

namespace digzip::format {

namespace {

constexpr std::uint8_t kSignature[8] = {'R', 'a', 'r', '!', 0x1A, 0x07, 0x01, 0x00};
constexpr std::size_t kMaxHeaderSize = 2u << 20;
constexpr std::size_t kMaxLinkTarget = 4096;

namespace header {
constexpr std::uint64_t Main = 1;
constexpr std::uint64_t File = 2;
constexpr std::uint64_t Encryption = 4;
constexpr std::uint64_t End = 5;

constexpr std::uint64_t HasExtra = 0x01;
constexpr std::uint64_t HasData = 0x02;
constexpr std::uint64_t SplitBefore = 0x08;
constexpr std::uint64_t SplitAfter = 0x10;
} // namespace header

namespace file {
constexpr std::uint64_t Directory = 0x01;
constexpr std::uint64_t HasTime = 0x02;
constexpr std::uint64_t HasCrc = 0x04;
constexpr std::uint64_t UnknownSize = 0x08;
} // namespace file

constexpr std::uint64_t kArchiveVolume = 0x01;
constexpr unsigned kHostUnix = 1;

[[noreturn]] void corrupt(const std::string& what)
{
    throw Error(ErrorCode::Corrupt, "rar: " + what);
}

// Bounded cursor over the bytes of one header.
class Cursor {
public:
    Cursor(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    std::size_t remaining() const { return size_ - pos_; }
    std::size_t position() const { return pos_; }

    std::uint64_t vint()
    {
        std::uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            const std::uint8_t b = byte();
            value |= std::uint64_t{b & 0x7Fu} << shift;
            if (!(b & 0x80)) {
                return value;
            }
        }
        corrupt("invalid variable-length number");
    }

    std::uint8_t byte()
    {
        if (pos_ >= size_) {
            corrupt("truncated header");
        }
        return data_[pos_++];
    }

    const std::uint8_t* take(std::uint64_t n)
    {
        if (n > remaining()) {
            corrupt("truncated header");
        }
        const std::uint8_t* p = data_ + pos_;
        pos_ += static_cast<std::size_t>(n);
        return p;
    }

    std::uint32_t u32() { return le32(take(4)); }
    std::uint64_t u64() { return le64(take(8)); }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
};

struct EncryptionParams {
    unsigned log2_count = 0;
    std::uint8_t salt[16] = {};
    std::uint8_t iv[16] = {};
    bool has_check = false;
    std::array<std::uint8_t, 8> check{};
    bool tweaked = false; // checksums replaced by HMAC values
};

// Encryption parameters, as in the archive encryption header and the file encryption record.
// `with_iv` is false for the archive header, which has no IV.
EncryptionParams read_encryption(Cursor& c, bool with_iv)
{
    EncryptionParams p;
    if (c.vint() != 0) {
        throw Error(ErrorCode::Unsupported, "rar: unknown encryption version");
    }
    const std::uint64_t flags = c.vint();
    p.log2_count = c.byte();
    std::memcpy(p.salt, c.take(16), 16);
    if (with_iv) {
        std::memcpy(p.iv, c.take(16), 16);
    }
    if (flags & 0x01) {
        const std::uint8_t* check = c.take(8);
        const std::uint8_t* sum = c.take(4);
        crypto::Sha256 sha;
        sha.update(check, 8);
        const auto digest = sha.finish();
        // A check value whose own checksum is wrong is ignored rather than trusted.
        if (std::memcmp(digest.data(), sum, 4) == 0) {
            p.has_check = true;
            std::memcpy(p.check.data(), check, 8);
        }
    }
    p.tweaked = (flags & 0x02) != 0;
    return p;
}

struct RarItem {
    std::string name;
    bool directory = false;
    std::optional<std::uint64_t> size;
    std::uint64_t attributes = 0;
    unsigned host = 0;
    std::optional<Timestamp> mtime;
    std::optional<std::uint32_t> crc;
    std::optional<std::array<std::uint8_t, 32>> blake2;
    std::uint64_t compression = 0;
    std::uint64_t data_offset = 0;
    std::uint64_t packed = 0;
    std::optional<EncryptionParams> encryption;
    unsigned redirect = 0; // 1 Unix link, 2 Windows link, 3 junction, 4 hard link, 5 file copy
    std::string target;
    bool split = false;

    unsigned method() const { return static_cast<unsigned>((compression >> 7) & 7); }
    bool solid() const { return (compression & 0x40) != 0; }
};

void read_extra(RarItem& item, Cursor& c)
{
    while (c.remaining() > 0) {
        const std::uint64_t size = c.vint();
        Cursor record(c.take(size), static_cast<std::size_t>(size));
        switch (record.vint()) {
        case 1:
            item.encryption = read_encryption(record, true);
            break;
        case 2:
            if (record.vint() == 0) {
                std::array<std::uint8_t, 32> digest{};
                std::memcpy(digest.data(), record.take(32), 32);
                item.blake2 = digest;
            }
            break;
        case 3: { // times
            const std::uint64_t flags = record.vint();
            const bool unix_format = (flags & 0x01) != 0;
            if (flags & 0x02) {
                item.mtime = unix_format ? Timestamp{record.u32(), 0} : from_filetime(record.u64());
                const unsigned others = ((flags & 0x04) ? 1u : 0u) + ((flags & 0x08) ? 1u : 0u);
                if (unix_format && (flags & 0x10)) {
                    record.take(4 * others);
                    item.mtime->nanoseconds = record.u32() % 1'000'000'000;
                }
            }
            break;
        }
        case 5: { // redirection
            item.redirect = static_cast<unsigned>(record.vint());
            record.vint(); // flags: target is a directory
            const std::uint64_t length = record.vint();
            if (length > kMaxLinkTarget) {
                corrupt("link target too long");
            }
            const std::uint8_t* name = record.take(length);
            item.target = text::to_utf8_or_latin1(std::string(reinterpret_cast<const char*>(name), length));
            break;
        }
        default:
            break; // version, owner, service data
        }
    }
}

// Size and checksum check for one file. With "tweaked" checksums (encrypted files), the stored
// CRC-32 and BLAKE2sp values are HMAC-SHA256 values of the real ones.
class RarCheckedStream final : public io::InputStream {
public:
    RarCheckedStream(io::StreamPtr upstream, const RarItem& item, const std::optional<codec::Key256>& hash_key)
        : upstream_(std::move(upstream)), size_(item.size), crc_(item.crc), blake2_(item.blake2), hash_key_(hash_key)
    {
    }

    std::size_t read(std::uint8_t* data, std::size_t size) override
    {
        if (done_ || size == 0) {
            return 0;
        }
        std::size_t wanted = size;
        if (size_) {
            wanted = static_cast<std::size_t>(std::min<std::uint64_t>(size, *size_ - produced_));
        }
        const std::size_t n = wanted == 0 ? 0 : upstream_->read(data, wanted);
        if (n == 0) {
            if (size_ && produced_ != *size_) {
                corrupt("the file holds less data than declared");
            }
            verify();
            done_ = true;
            return 0;
        }
        computed_crc_ = static_cast<std::uint32_t>(crc32(computed_crc_, data, static_cast<uInt>(n)));
        if (blake2_) {
            hash_.update(data, n);
        }
        produced_ += n;
        return n;
    }

private:
    void verify()
    {
        if (crc_) {
            std::uint32_t value = computed_crc_;
            if (hash_key_) {
                std::uint8_t raw[4];
                for (int i = 0; i < 4; ++i) {
                    raw[i] = static_cast<std::uint8_t>(value >> (8 * i));
                }
                const auto digest = mac(raw, 4);
                value = 0;
                for (std::size_t i = 0; i < digest.size(); ++i) {
                    value ^= std::uint32_t{digest[i]} << ((i & 3) * 8);
                }
            }
            if (value != *crc_) {
                throw Error(ErrorCode::Checksum, "rar: CRC-32 mismatch, the file is corrupt");
            }
        }
        if (blake2_) {
            auto digest = hash_.finish();
            if (hash_key_) {
                const auto mac_value = mac(digest.data(), digest.size());
                std::copy(mac_value.begin(), mac_value.end(), digest.begin());
            }
            if (digest != *blake2_) {
                throw Error(ErrorCode::Checksum, "rar: BLAKE2sp mismatch, the file is corrupt");
            }
        }
    }

    std::vector<std::uint8_t> mac(const std::uint8_t* data, std::size_t size) const
    {
        crypto::Hmac hmac(crypto::Hash::Sha256, hash_key_->data(), hash_key_->size());
        hmac.update(data, size);
        return hmac.finish();
    }

    io::StreamPtr upstream_;
    std::optional<std::uint64_t> size_;
    std::optional<std::uint32_t> crc_;
    std::optional<std::array<std::uint8_t, 32>> blake2_;
    std::optional<codec::Key256> hash_key_;
    std::uint64_t produced_ = 0;
    std::uint32_t computed_crc_ = 0;
    codec::Blake2sp hash_;
    bool done_ = false;
};

// Decoded bytes of the current file of a shared unpacker.
class UnpackerStream final : public io::InputStream {
public:
    explicit UnpackerStream(codec::Rar5Unpacker& unpacker) : unpacker_(unpacker) {}
    std::size_t read(std::uint8_t* data, std::size_t size) override { return unpacker_.read(data, size); }

private:
    codec::Rar5Unpacker& unpacker_;
};

class RarReader final : public ArchiveReader {
public:
    RarReader(io::SourcePtr source, Monitor& monitor) : source_(std::move(source)), monitor_(monitor)
    {
        read_headers();
    }

    ~RarReader() override
    {
        for (auto& entry : keys_) {
            crypto::wipe(&entry.second, sizeof entry.second);
        }
    }

    bool next(Entry& entry) override
    {
        finish_current();
        if (++index_ >= items_.size()) {
            return false;
        }
        const RarItem& item = items_[index_];
        entry = Entry{};
        entry.path = item.name;
        std::replace(entry.path.begin(), entry.path.end(), '\\', '/');
        entry.mtime = item.mtime;
        entry.packed_size = item.packed;
        entry.encrypted = item.encryption.has_value();
        entry.method = item.method() == 0 ? "stored" : "rar5";
        if (entry.encrypted) {
            entry.method += "+aes256";
        }
        if (item.crc && !(item.encryption && item.encryption->tweaked)) {
            entry.crc32 = item.crc;
        }
        if (item.host == kHostUnix) {
            entry.mode = static_cast<std::uint32_t>(item.attributes & 07777);
        }
        if (item.directory) {
            entry.type = EntryType::Directory;
        } else if (item.redirect >= 1 && item.redirect <= 3) {
            entry.type = EntryType::Symlink;
            entry.link_target = item.target;
        } else if (item.redirect == 4 || item.redirect == 5) {
            entry.type = EntryType::Hardlink;
            entry.link_target = item.target;
        } else if (item.host == kHostUnix && (item.attributes & 0170000) != 0 &&
                   (item.attributes & 0170000) != 0100000) {
            entry.type = EntryType::Special;
        } else {
            entry.type = EntryType::File;
            entry.size = item.size;
        }
        return true;
    }

    io::InputStream& data() override
    {
        if (!stream_) {
            stream_ = open(items_[index_]);
        }
        return *stream_;
    }

    bool independent_entries() const override { return !solid_; }

private:
    // --- headers ------------------------------------------------------------------------------

    struct Header {
        std::uint64_t type = 0;
        std::uint64_t flags = 0;
        std::vector<std::uint8_t> body;  // type-specific fields followed by the extra area
        std::uint64_t extra_size = 0;
        std::uint64_t data_offset = 0;
        std::uint64_t data_size = 0;
    };

    void read_headers()
    {
        std::uint8_t signature[8];
        if (source_->size() < sizeof signature) {
            corrupt("the file is too small to be a RAR archive");
        }
        source_->read_at(0, signature, sizeof signature);
        if (std::memcmp(signature, kSignature, sizeof kSignature) != 0) {
            throw Error(ErrorCode::Unsupported, "RAR 4.x archives are not supported yet");
        }
        std::uint64_t offset = sizeof signature;
        const std::uint64_t max_entries = monitor_.limits().max_entries;
        while (offset < source_->size()) {
            const Header h = read_header(offset);
            Cursor body(h.body.data(), h.body.size());
            switch (h.type) {
            case header::Main:
                if (body.vint() & kArchiveVolume) {
                    throw Error(ErrorCode::Unsupported, "rar: multi-part archives are not supported yet");
                }
                break;
            case header::Encryption: {
                header_encryption_ = read_encryption(body, false);
                header_keys_ = keys_for(*header_encryption_, true);
                break;
            }
            case header::File:
                if (max_entries != 0 && items_.size() >= max_entries) {
                    throw Error(ErrorCode::LimitExceeded, "the archive has more than " + std::to_string(max_entries) +
                                                              " entries (limit: max_entries)");
                }
                items_.push_back(read_file(h));
                solid_ = solid_ || items_.back().solid();
                break;
            case header::End:
                return;
            default:
                break; // service headers: comments, quick open data, recovery record...
            }
            offset = h.data_offset + h.data_size;
        }
    }

    Header read_header(std::uint64_t offset)
    {
        const std::uint64_t file_size = source_->size();
        Header h;
        std::vector<std::uint8_t> raw;
        std::uint64_t end = 0;
        if (header_encryption_) {
            // Encrypted headers: a 16-byte IV, then the header padded to whole AES blocks.
            std::uint8_t iv[16];
            if (offset + 32 > file_size) {
                corrupt("truncated archive");
            }
            source_->read_at(offset, iv, 16);
            raw.resize(16);
            source_->read_at(offset + 16, raw.data(), 16);
            std::vector<std::uint8_t> first = raw;
            codec::decrypt_aes256_cbc(header_keys_->key, iv, first.data(), first.size());
            const std::uint64_t total = 4 + header_length(first);
            const std::uint64_t padded = (total + 15) / 16 * 16;
            if (padded > kMaxHeaderSize || offset + 16 + padded > file_size) {
                corrupt("truncated archive");
            }
            raw.resize(static_cast<std::size_t>(padded));
            source_->read_at(offset + 16, raw.data(), raw.size());
            codec::decrypt_aes256_cbc(header_keys_->key, iv, raw.data(), raw.size());
            raw.resize(static_cast<std::size_t>(total));
            end = offset + 16 + padded;
        } else {
            std::uint8_t start[14] = {};
            const auto available = static_cast<std::size_t>(std::min<std::uint64_t>(sizeof start, file_size - offset));
            source_->read_at(offset, start, available);
            const std::uint64_t total = 4 + header_length(std::vector<std::uint8_t>(start, start + available));
            if (total > kMaxHeaderSize || offset + total > file_size) {
                corrupt("truncated archive");
            }
            raw.resize(static_cast<std::size_t>(total));
            source_->read_at(offset, raw.data(), raw.size());
            end = offset + total;
        }

        if (le32(raw.data()) != crc32(0, raw.data() + 4, static_cast<uInt>(raw.size() - 4))) {
            throw Error(ErrorCode::Checksum, "rar: header CRC mismatch");
        }
        Cursor c(raw.data() + 4, raw.size() - 4);
        c.vint(); // header size, already used
        h.type = c.vint();
        h.flags = c.vint();
        if (h.flags & header::HasExtra) {
            h.extra_size = c.vint();
        }
        if (h.flags & header::HasData) {
            h.data_size = c.vint();
        }
        const std::uint8_t* body = c.take(c.remaining());
        h.body.assign(body, static_cast<const std::uint8_t*>(raw.data() + raw.size()));
        if (h.extra_size > h.body.size()) {
            corrupt("invalid extra area size");
        }
        h.data_offset = end;
        if (h.data_size > file_size || h.data_offset > file_size - h.data_size) {
            corrupt("truncated archive");
        }
        return h;
    }

    // Size of a header from its first bytes: CRC (4) + size field + the size it gives.
    static std::uint64_t header_length(const std::vector<std::uint8_t>& start)
    {
        if (start.size() < 5) {
            corrupt("truncated header");
        }
        Cursor c(start.data() + 4, start.size() - 4);
        const std::size_t before = c.position();
        const std::uint64_t size = c.vint();
        if (size == 0 || size > kMaxHeaderSize) {
            corrupt("invalid header size");
        }
        return (c.position() - before) + size;
    }

    RarItem read_file(const Header& h)
    {
        RarItem item;
        Cursor c(h.body.data(), h.body.size() - static_cast<std::size_t>(h.extra_size));
        const std::uint64_t flags = c.vint();
        const std::uint64_t size = c.vint();
        item.attributes = c.vint();
        if (flags & file::HasTime) {
            item.mtime = Timestamp{c.u32(), 0};
        }
        if (flags & file::HasCrc) {
            item.crc = c.u32();
        }
        item.compression = c.vint();
        item.host = static_cast<unsigned>(c.vint());
        const std::uint64_t name_length = c.vint();
        const std::uint8_t* name = c.take(name_length);
        item.name = text::to_utf8_or_latin1(std::string(reinterpret_cast<const char*>(name), name_length));
        item.directory = (flags & file::Directory) != 0;
        if (!(flags & file::UnknownSize)) {
            item.size = size;
        }
        Cursor extra(h.body.data() + h.body.size() - h.extra_size, static_cast<std::size_t>(h.extra_size));
        read_extra(item, extra);
        item.data_offset = h.data_offset;
        item.packed = h.data_size;
        item.split = (h.flags & (header::SplitBefore | header::SplitAfter)) != 0;
        return item;
    }

    // --- passwords ----------------------------------------------------------------------------

    // Keys for one set of parameters, trying the passwords in order (last working one first).
    const codec::Rar5Keys* keys_for(const EncryptionParams& p, bool headers)
    {
        const std::vector<std::string>& passwords = monitor_.passwords();
        if (passwords.empty()) {
            throw Error(ErrorCode::PasswordRequired,
                        headers ? "rar: the file names are encrypted, a password is needed"
                                : "rar: the file is encrypted, a password is needed");
        }
        for (std::size_t k = 0; k < passwords.size(); ++k) {
            const std::size_t i = (preferred_password_ + k) % passwords.size();
            std::vector<std::uint8_t> id(p.salt, p.salt + 16);
            id.push_back(static_cast<std::uint8_t>(p.log2_count));
            id.push_back(static_cast<std::uint8_t>(i));
            auto it = keys_.find(id);
            if (it == keys_.end()) {
                it = keys_.emplace(id, codec::derive_rar5_keys(passwords[i], p.salt, p.log2_count)).first;
            }
            if (!p.has_check || it->second.check == p.check) {
                preferred_password_ = i;
                return &it->second;
            }
        }
        throw Error(ErrorCode::WrongPassword, "rar: none of the given passwords decrypts the data");
    }

    // --- data ---------------------------------------------------------------------------------

    io::StreamPtr open(const RarItem& item)
    {
        if (broken_ && item.solid()) {
            corrupt("an earlier error damaged this solid archive");
        }
        if (item.split) {
            throw Error(ErrorCode::Unsupported, "rar: files split across volumes are not supported yet");
        }
        if ((item.compression & 0x3F) != 0) {
            throw Error(ErrorCode::Unsupported, "rar: this compression version (RAR 7 large dictionaries) is not supported yet");
        }
        io::StreamPtr raw = std::make_unique<io::SourceStream>(source_, item.data_offset, item.packed);
        std::optional<codec::Key256> hash_key;
        if (item.encryption) {
            const codec::Rar5Keys* keys = keys_for(*item.encryption, false);
            if (item.encryption->tweaked) {
                hash_key = keys->hash_key;
            }
            raw = codec::decrypt_aes256_cbc(std::move(raw), keys->key, item.encryption->iv);
        }

        io::StreamPtr decoded;
        if (item.method() == 0) {
            decoded = std::move(raw);
        } else {
            const std::uint64_t dictionary = std::uint64_t{0x20000} << ((item.compression >> 10) & 0x1F);
            const std::uint64_t limit = monitor_.limits().max_memory;
            if (limit != 0 && dictionary > limit) {
                throw Error(ErrorCode::LimitExceeded, "rar: the file needs a " + std::to_string(dictionary >> 20) +
                                                          " MiB dictionary (limit: max_memory)");
            }
            if (item.solid() && !unpacker_started_) {
                corrupt("solid file without a preceding file");
            }
            unpacker_.start(std::move(raw), item.size, dictionary, item.solid());
            unpacker_started_ = true;
            decoded = std::make_unique<UnpackerStream>(unpacker_);
        }
        decoded_ = true;
        auto counted = std::make_unique<MonitoredStream>(std::move(decoded), monitor_);
        return std::make_unique<GuardedStream>(std::make_unique<RarCheckedStream>(std::move(counted), item, hash_key));
    }

    // In a solid archive, every compressed file must be decoded, even when it is not extracted.
    void finish_current()
    {
        if (index_ < items_.size() && solid_ && !broken_) {
            const RarItem& item = items_[index_];
            if (item.method() != 0 && !item.directory && item.redirect == 0) {
                try {
                    io::InputStream& stream = data();
                    std::uint8_t buffer[16384];
                    while (stream.read(buffer, sizeof buffer) > 0) {
                    }
                } catch (const Error&) {
                    broken_ = true;
                }
            }
        }
        stream_.reset();
        decoded_ = false;
    }

    io::SourcePtr source_;
    Monitor& monitor_;
    std::vector<RarItem> items_;
    bool solid_ = false;
    std::optional<EncryptionParams> header_encryption_;
    const codec::Rar5Keys* header_keys_ = nullptr;
    std::map<std::vector<std::uint8_t>, codec::Rar5Keys> keys_;
    std::size_t preferred_password_ = 0;

    std::size_t index_ = static_cast<std::size_t>(-1);
    io::StreamPtr stream_;
    codec::Rar5Unpacker unpacker_;
    bool unpacker_started_ = false;
    bool decoded_ = false;
    bool broken_ = false;
};

} // namespace

std::unique_ptr<ArchiveReader> make_rar_reader(io::SourcePtr source, Monitor& monitor)
{
    return std::make_unique<RarReader>(std::move(source), monitor);
}

} // namespace digzip::format
