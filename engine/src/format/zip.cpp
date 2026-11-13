#include "codec/decoders.hpp"
#include "format/common.hpp"
#include "format/reader.hpp"
#include "text.hpp"

#include <digzip/error.hpp>

#include <lzma.h>
#include <zlib.h>

#include <algorithm>
#include <ctime>
#include <numeric>

namespace digzip::format {

namespace {

constexpr std::uint32_t kLocalSignature = 0x04034B50;
constexpr std::uint32_t kCentralSignature = 0x02014B50;
constexpr std::uint32_t kEndSignature = 0x06054B50;
constexpr std::uint32_t kEnd64Signature = 0x06064B50;
constexpr std::uint32_t kLocator64Signature = 0x07064B50;

constexpr std::size_t kLocalHeaderSize = 30;
constexpr std::size_t kCentralHeaderSize = 46;
constexpr std::size_t kEndSize = 22;
constexpr std::size_t kLocatorSize = 20;
constexpr std::size_t kEnd64Size = 56;
constexpr std::size_t kMaxComment = 0xFFFF;
constexpr std::size_t kMaxLinkTarget = 4096;

constexpr std::uint16_t kFlagEncrypted = 0x0001;
constexpr std::uint16_t kFlagStrongEncryption = 0x0040;
constexpr std::uint16_t kFlagUtf8 = 0x0800;

constexpr std::uint8_t kHostUnix = 3;
constexpr std::uint8_t kHostMacOs = 19;

[[noreturn]] void corrupt(const std::string& what)
{
    throw Error(ErrorCode::Corrupt, "zip: " + what);
}

// MS-DOS date and time, in local time.
std::optional<Timestamp> dos_time(std::uint16_t time, std::uint16_t date)
{
    std::tm tm{};
    tm.tm_year = (date >> 9) + 80;
    tm.tm_mon = ((date >> 5) & 0x0F) - 1;
    tm.tm_mday = date & 0x1F;
    tm.tm_hour = time >> 11;
    tm.tm_min = (time >> 5) & 0x3F;
    tm.tm_sec = (time & 0x1F) * 2;
    tm.tm_isdst = -1;
    if (tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday == 0) {
        return std::nullopt;
    }
    const std::time_t t = std::mktime(&tm);
    if (t == static_cast<std::time_t>(-1)) {
        return std::nullopt;
    }
    return Timestamp{static_cast<std::int64_t>(t), 0};
}

struct ZipItem {
    std::string name;
    std::uint16_t made_by = 0;
    std::uint16_t flags = 0;
    std::uint16_t method = 0;
    std::uint32_t crc = 0;
    std::uint64_t packed = 0;
    std::uint64_t size = 0;
    std::uint64_t offset = 0; // local header, before the prepended-data correction
    std::uint32_t external = 0;
    std::optional<Timestamp> mtime;
};

// Reads the variable fields of a central directory record from its extra data.
void apply_extra(ZipItem& item, const std::uint8_t* extra, std::size_t size, const std::string& raw_name)
{
    std::optional<Timestamp> ntfs_time;
    std::optional<Timestamp> unix_time;
    std::size_t pos = 0;
    while (pos + 4 <= size) {
        const std::uint16_t id = le16(extra + pos);
        const std::uint16_t length = le16(extra + pos + 2);
        const std::uint8_t* data = extra + pos + 4;
        if (pos + 4 + length > size) {
            break;
        }
        switch (id) {
        case 0x0001: { // Zip64: only the fields saturated in the record are present, in this order
            std::size_t at = 0;
            const auto take = [&](std::uint64_t& field) {
                if (at + 8 > length) {
                    corrupt("truncated Zip64 extra field");
                }
                field = le64(data + at);
                at += 8;
            };
            if (item.size == 0xFFFFFFFF) {
                take(item.size);
            }
            if (item.packed == 0xFFFFFFFF) {
                take(item.packed);
            }
            if (item.offset == 0xFFFFFFFF) {
                take(item.offset);
            }
            break;
        }
        case 0x000A: // NTFS times
            for (std::size_t at = 4; at + 4 <= length;) {
                const std::uint16_t tag = le16(data + at);
                const std::uint16_t tag_size = le16(data + at + 2);
                if (tag == 1 && tag_size >= 8 && at + 4 + 8 <= length) {
                    ntfs_time = from_filetime(le64(data + at + 4));
                }
                at += 4 + tag_size;
            }
            break;
        case 0x5455: // Info-ZIP extended timestamp
            if (length >= 5 && (data[0] & 1)) {
                unix_time = Timestamp{static_cast<std::int32_t>(le32(data + 1)), 0};
            }
            break;
        case 0x7075: // Info-ZIP Unicode path, valid only if it matches the stored name
            if (length >= 5 && data[0] == 1 && le32(data + 1) == crc32(0, reinterpret_cast<const Bytef*>(raw_name.data()),
                                                                      static_cast<uInt>(raw_name.size()))) {
                std::string unicode(reinterpret_cast<const char*>(data + 5), length - 5u);
                if (text::is_valid_utf8(unicode)) {
                    item.name = std::move(unicode);
                }
            }
            break;
        default:
            break;
        }
        pos += 4u + length;
    }
    if (ntfs_time) {
        item.mtime = ntfs_time;
    } else if (unix_time) {
        item.mtime = unix_time;
    }
}

std::string decode_name(const std::string& raw, std::uint16_t flags)
{
    if ((flags & kFlagUtf8) && text::is_valid_utf8(raw)) {
        return raw;
    }
    // Without the UTF-8 flag the standard says CP437, but many tools write UTF-8 anyway.
    // CP437 text is almost never valid UTF-8, so valid UTF-8 is taken as such.
    return text::is_valid_utf8(raw) ? raw : text::cp437_to_utf8(raw);
}

const char* method_name(std::uint16_t method)
{
    switch (method) {
    case 0:
        return "stored";
    case 8:
        return "deflate";
    case 9:
        return "deflate64";
    case 12:
        return "bzip2";
    case 14:
        return "lzma";
    case 93:
        return "zstd";
    case 95:
        return "xz";
    case 98:
        return "ppmd";
    case 99:
        return "aes";
    default:
        return nullptr;
    }
}

class ZipReader final : public ArchiveReader {
public:
    ZipReader(io::SourcePtr source, Budget& budget) : source_(std::move(source)), budget_(budget)
    {
        read_directory();
    }

    bool next(Entry& entry) override
    {
        stream_.reset();
        if (++index_ >= items_.size()) {
            return false;
        }
        const ZipItem& item = items_[index_];
        entry = Entry{};
        entry.path = item.name;
        std::replace(entry.path.begin(), entry.path.end(), '\\', '/');
        entry.size = item.size;
        entry.packed_size = item.packed;
        entry.crc32 = item.crc;
        entry.mtime = item.mtime;
        entry.encrypted = (item.flags & kFlagEncrypted) != 0;
        const char* method = method_name(item.method);
        entry.method = method != nullptr ? method : "method " + std::to_string(item.method);
        entry.type = entry_type(item, entry.mode);
        if (entry.path.size() > 1 && entry.path.back() == '/') {
            entry.type = EntryType::Directory;
            entry.path.pop_back();
        }
        if (entry.type != EntryType::File) {
            entry.size.reset();
        }
        if (entry.type == EntryType::Symlink) {
            entry.link_target = read_link_target();
        }
        return true;
    }

    io::InputStream& data() override
    {
        if (!stream_) {
            stream_ = open_entry(items_[index_]);
        }
        return *stream_;
    }

    bool independent_entries() const override { return true; }

private:
    void read_directory()
    {
        const std::uint64_t file_size = source_->size();
        if (file_size < kEndSize) {
            corrupt("the file is too small to be a ZIP archive");
        }

        // The end record sits in the last 22 bytes plus at most a 64 KiB comment.
        const auto tail_size = static_cast<std::size_t>(std::min<std::uint64_t>(file_size, kEndSize + kMaxComment));
        const std::uint64_t tail_start = file_size - tail_size;
        std::vector<std::uint8_t> tail(tail_size);
        source_->read_at(tail_start, tail.data(), tail.size());

        std::optional<std::size_t> found;
        for (std::size_t pos = tail_size - kEndSize + 1; pos-- > 0;) {
            if (le32(tail.data() + pos) != kEndSignature) {
                continue;
            }
            if (pos + kEndSize + le16(tail.data() + pos + 20) == tail_size) {
                found = pos; // exact match: the comment runs to the end of the file
                break;
            }
            if (!found) {
                found = pos;
            }
        }
        if (!found) {
            corrupt("end of central directory not found");
        }
        const std::uint8_t* end = tail.data() + *found;
        const std::uint64_t end_offset = tail_start + *found;

        std::uint32_t disk = le16(end + 4);
        std::uint32_t directory_disk = le16(end + 6);
        std::uint64_t count = le16(end + 10);
        std::uint64_t directory_size = le32(end + 12);
        std::uint64_t directory_offset = le32(end + 16);
        std::uint64_t directory_end = end_offset;

        if (end_offset >= kLocatorSize) {
            std::uint8_t locator[kLocatorSize];
            source_->read_at(end_offset - kLocatorSize, locator, kLocatorSize);
            if (le32(locator) == kLocator64Signature) {
                const std::uint64_t end64_offset = find_end64(le64(locator + 8), end_offset - kLocatorSize);
                std::uint8_t end64[kEnd64Size];
                source_->read_at(end64_offset, end64, kEnd64Size);
                disk = le32(end64 + 16);
                directory_disk = le32(end64 + 20);
                count = le64(end64 + 32);
                directory_size = le64(end64 + 40);
                directory_offset = le64(end64 + 48);
                directory_end = end64_offset;
            }
        }
        if (disk != 0 || directory_disk != 0) {
            throw Error(ErrorCode::Unsupported, "zip: split (multi-part) archives are not supported yet");
        }

        // Data prepended to the archive (self-extracting executables) shifts every offset.
        if (directory_size > directory_end || directory_offset > directory_end - directory_size) {
            corrupt("the central directory lies outside the archive");
        }
        directory_start_ = directory_end - directory_size;
        shift_ = directory_start_ - directory_offset;

        if (count > directory_size / kCentralHeaderSize) {
            corrupt("the entry count does not match the central directory size");
        }
        const std::uint64_t max_entries = budget_.limits().max_entries;
        if (max_entries != 0 && count > max_entries) {
            throw Error(ErrorCode::LimitExceeded, "the archive has more than " + std::to_string(max_entries) +
                                                      " entries (limit: max_entries)");
        }

        io::SourceStream directory(source_, directory_start_, directory_size);
        items_.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t i = 0; i < count; ++i) {
            items_.push_back(read_central_record(directory));
        }
        check_layout();
    }

    std::uint64_t find_end64(std::uint64_t recorded, std::uint64_t locator_offset) const
    {
        std::uint8_t signature[4];
        if (recorded + kEnd64Size <= locator_offset) {
            source_->read_at(recorded, signature, 4);
            if (le32(signature) == kEnd64Signature) {
                return recorded;
            }
        }
        // Prepended data: the Zip64 end record normally sits right before the locator.
        if (locator_offset >= kEnd64Size) {
            source_->read_at(locator_offset - kEnd64Size, signature, 4);
            if (le32(signature) == kEnd64Signature) {
                return locator_offset - kEnd64Size;
            }
        }
        corrupt("Zip64 end of central directory not found");
    }

    ZipItem read_central_record(io::InputStream& directory)
    {
        std::uint8_t h[kCentralHeaderSize];
        directory.read_exact(h, sizeof h);
        if (le32(h) != kCentralSignature) {
            corrupt("bad central directory record");
        }
        ZipItem item;
        item.made_by = le16(h + 4);
        item.flags = le16(h + 8);
        item.method = le16(h + 10);
        item.mtime = dos_time(le16(h + 12), le16(h + 14));
        item.crc = le32(h + 16);
        item.packed = le32(h + 20);
        item.size = le32(h + 24);
        const std::uint16_t name_length = le16(h + 28);
        const std::uint16_t extra_length = le16(h + 30);
        const std::uint16_t comment_length = le16(h + 32);
        item.external = le32(h + 38);
        item.offset = le32(h + 42);

        std::string raw_name(name_length, '\0');
        directory.read_exact(reinterpret_cast<std::uint8_t*>(raw_name.data()), raw_name.size());
        std::vector<std::uint8_t> extra(extra_length);
        directory.read_exact(extra.data(), extra.size());
        if (directory.skip(comment_length) != comment_length) {
            corrupt("truncated central directory");
        }
        item.name = decode_name(raw_name, item.flags);
        apply_extra(item, extra.data(), extra.size(), raw_name);
        return item;
    }

    // Rejects entries whose data would overlap: the trick behind "overlapping file" zip bombs,
    // where many entries reuse the same compressed bytes.
    void check_layout() const
    {
        std::vector<std::size_t> order(items_.size());
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::sort(order.begin(), order.end(),
                  [&](std::size_t a, std::size_t b) { return items_[a].offset < items_[b].offset; });
        for (std::size_t i = 0; i < order.size(); ++i) {
            const ZipItem& item = items_[order[i]];
            const std::uint64_t start = item.offset + shift_;
            const std::uint64_t limit =
                i + 1 < order.size() ? items_[order[i + 1]].offset + shift_ : directory_start_;
            if (item.offset > directory_start_ || item.packed > directory_start_ ||
                start + kLocalHeaderSize + item.packed > limit) {
                throw Error(ErrorCode::Security,
                            "zip: entries overlap or lie outside the archive data, as in zip bombs");
            }
        }
    }

    static EntryType entry_type(const ZipItem& item, std::optional<std::uint32_t>& mode)
    {
        const auto host = static_cast<std::uint8_t>(item.made_by >> 8);
        const std::uint32_t unix_mode = item.external >> 16;
        if ((host == kHostUnix || host == kHostMacOs) && unix_mode != 0) {
            mode = unix_mode & 07777;
            switch (unix_mode & 0170000) {
            case 0120000:
                return EntryType::Symlink;
            case 0040000:
                return EntryType::Directory;
            case 0100000:
            case 0:
                return EntryType::File;
            default:
                return EntryType::Special;
            }
        }
        return (item.external & 0x10) ? EntryType::Directory : EntryType::File;
    }

    // A symbolic link stores its target as the entry data.
    std::string read_link_target()
    {
        try {
            io::InputStream& stream = data();
            std::string target(kMaxLinkTarget + 1, '\0');
            target.resize(stream.read_up_to(reinterpret_cast<std::uint8_t*>(target.data()), target.size()));
            if (target.size() > kMaxLinkTarget) {
                return {};
            }
            return text::to_utf8_or_latin1(target);
        } catch (const Error&) {
            return {}; // reported when the link is extracted: an empty target is refused
        }
    }

    io::StreamPtr open_entry(const ZipItem& item)
    {
        if (item.flags & (kFlagEncrypted | kFlagStrongEncryption)) {
            throw Error(ErrorCode::Unsupported, "zip: encrypted entries are not supported yet");
        }
        std::uint8_t local[kLocalHeaderSize];
        const std::uint64_t local_offset = item.offset + shift_;
        source_->read_at(local_offset, local, sizeof local);
        if (le32(local) != kLocalSignature) {
            corrupt("bad local header");
        }
        const std::uint64_t data_offset = local_offset + kLocalHeaderSize + le16(local + 26) + le16(local + 28);
        if (data_offset > directory_start_ || item.packed > directory_start_ - data_offset) {
            corrupt("entry data lies outside the archive");
        }

        auto raw = std::make_unique<io::SourceStream>(source_, data_offset, item.packed);
        auto decoded = std::make_unique<BudgetStream>(decoder(item, std::move(raw)), budget_);
        return std::make_unique<CheckedStream>(std::move(decoded), item.size, item.crc);
    }

    io::StreamPtr decoder(const ZipItem& item, io::StreamPtr raw) const
    {
        const std::uint64_t memory = budget_.limits().max_memory;
        switch (item.method) {
        case 0:
            return raw;
        case 8:
            return codec::decode_deflate(std::move(raw));
        case 9:
            return codec::decode_inflate(std::move(raw), true);
        case 12:
            return codec::decode_bzip2(std::move(raw));
        case 14: {
            // 2-byte version, 2-byte properties size, then the LZMA properties.
            std::uint8_t header[4];
            raw->read_exact(header, sizeof header);
            codec::LzmaFilter lzma{LZMA_FILTER_LZMA1, std::vector<std::uint8_t>(le16(header + 2))};
            if (lzma.properties.size() != 5) {
                corrupt("invalid LZMA properties");
            }
            raw->read_exact(lzma.properties.data(), lzma.properties.size());
            return codec::decode_lzma_raw(std::move(raw), {lzma}, item.size, memory);
        }
        case 93:
            return codec::decode_zstd(std::move(raw), memory);
        case 98:
            return codec::decode_ppmd8_zip(std::move(raw), item.size, memory);
        case 95:
            return codec::decode_xz(std::move(raw), memory);
        default: {
            const char* name = method_name(item.method);
            throw Error(ErrorCode::Unsupported, std::string("zip: compression method ") +
                                                    (name != nullptr ? name : std::to_string(item.method)) +
                                                    " is not supported");
        }
        }
    }

    io::SourcePtr source_;
    Budget& budget_;
    std::vector<ZipItem> items_;
    std::uint64_t directory_start_ = 0;
    std::uint64_t shift_ = 0;
    std::size_t index_ = static_cast<std::size_t>(-1);
    io::StreamPtr stream_;
};

} // namespace

bool has_zip_directory(const io::Source& source)
{
    const std::uint64_t size = source.size();
    if (size < kEndSize) {
        return false;
    }
    const auto tail_size = static_cast<std::size_t>(std::min<std::uint64_t>(size, kEndSize + kMaxComment));
    std::vector<std::uint8_t> tail(tail_size);
    source.read_at(size - tail_size, tail.data(), tail.size());
    for (std::size_t pos = tail_size - kEndSize + 1; pos-- > 0;) {
        if (le32(tail.data() + pos) == kEndSignature && pos + kEndSize + le16(tail.data() + pos + 20) == tail_size) {
            return true;
        }
    }
    return false;
}

std::unique_ptr<ArchiveReader> make_zip_reader(io::SourcePtr source, Budget& budget)
{
    return std::make_unique<ZipReader>(std::move(source), budget);
}

} // namespace digzip::format
