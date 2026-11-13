#include "codec/decoders.hpp"
#include "format/common.hpp"
#include "format/reader.hpp"
#include "text.hpp"

#include <digzip/error.hpp>

#include <lzma.h>
#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <exception>

namespace digzip::format {

namespace {

constexpr std::uint8_t kSignature[6] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};
constexpr std::size_t kSignatureHeaderSize = 32;
constexpr std::uint64_t kMaxHeaderSize = 256ull << 20;
constexpr std::size_t kMaxCoders = 64;
constexpr std::size_t kMaxLinkTarget = 4096;
constexpr std::uint32_t kAttributeDirectory = 0x10;
constexpr std::uint32_t kAttributeUnixExtension = 0x8000;

namespace id {
constexpr std::uint8_t End = 0x00;
constexpr std::uint8_t Header = 0x01;
constexpr std::uint8_t ArchiveProperties = 0x02;
constexpr std::uint8_t AdditionalStreamsInfo = 0x03;
constexpr std::uint8_t MainStreamsInfo = 0x04;
constexpr std::uint8_t FilesInfo = 0x05;
constexpr std::uint8_t PackInfo = 0x06;
constexpr std::uint8_t UnpackInfo = 0x07;
constexpr std::uint8_t SubStreamsInfo = 0x08;
constexpr std::uint8_t Size = 0x09;
constexpr std::uint8_t Crc = 0x0A;
constexpr std::uint8_t Folder = 0x0B;
constexpr std::uint8_t CodersUnpackSize = 0x0C;
constexpr std::uint8_t NumUnpackStream = 0x0D;
constexpr std::uint8_t EmptyStream = 0x0E;
constexpr std::uint8_t EmptyFile = 0x0F;
constexpr std::uint8_t Anti = 0x10;
constexpr std::uint8_t Name = 0x11;
constexpr std::uint8_t MTime = 0x14;
constexpr std::uint8_t WinAttributes = 0x15;
constexpr std::uint8_t EncodedHeader = 0x17;
} // namespace id

namespace method {
constexpr std::uint64_t Copy = 0x00;
constexpr std::uint64_t Delta = 0x03;
constexpr std::uint64_t Arm64 = 0x0A;
constexpr std::uint64_t RiscV = 0x0B;
constexpr std::uint64_t Lzma2 = 0x21;
constexpr std::uint64_t Lzma = 0x030101;
constexpr std::uint64_t X86 = 0x03030103;
constexpr std::uint64_t Bcj2 = 0x0303011B;
constexpr std::uint64_t PowerPc = 0x03030205;
constexpr std::uint64_t Ia64 = 0x03030401;
constexpr std::uint64_t Arm = 0x03030501;
constexpr std::uint64_t ArmThumb = 0x03030701;
constexpr std::uint64_t Sparc = 0x03030805;
constexpr std::uint64_t Ppmd = 0x030401;
constexpr std::uint64_t Deflate = 0x040108;
constexpr std::uint64_t Deflate64 = 0x040109;
constexpr std::uint64_t Bzip2 = 0x040202;
constexpr std::uint64_t Zstd = 0x04F71101;
constexpr std::uint64_t Aes = 0x06F10701;
} // namespace method

[[noreturn]] void corrupt(const std::string& what)
{
    throw Error(ErrorCode::Corrupt, "7z: " + what);
}

// Cursor over a decoded header. Counts are checked against the bytes left, so a hostile header
// cannot make the reader allocate more than its own size allows.
class HeaderReader {
public:
    HeaderReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    std::size_t remaining() const { return size_ - pos_; }

    std::uint8_t byte()
    {
        if (pos_ >= size_) {
            corrupt("truncated header");
        }
        return data_[pos_++];
    }

    // 7z variable-length number: the leading 1 bits of the first byte count the extra bytes.
    std::uint64_t number()
    {
        const std::uint8_t first = byte();
        std::uint8_t mask = 0x80;
        std::uint64_t value = 0;
        for (int i = 0; i < 8; ++i) {
            if ((first & mask) == 0) {
                const std::uint64_t high = first & (mask - 1u);
                return value | (high << (8 * i));
            }
            value |= static_cast<std::uint64_t>(byte()) << (8 * i);
            mask >>= 1;
        }
        return value;
    }

    std::size_t count()
    {
        const std::uint64_t n = number();
        if (n > remaining()) {
            corrupt("invalid item count");
        }
        return static_cast<std::size_t>(n);
    }

    std::uint32_t u32() { return le32(take(4)); }
    std::uint64_t u64() { return le64(take(8)); }

    const std::uint8_t* take(std::uint64_t n)
    {
        if (n > remaining()) {
            corrupt("truncated header");
        }
        const std::uint8_t* p = data_ + pos_;
        pos_ += static_cast<std::size_t>(n);
        return p;
    }

    std::vector<bool> bits(std::size_t n)
    {
        std::vector<bool> out(n);
        std::uint8_t current = 0;
        std::uint8_t mask = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (mask == 0) {
                current = byte();
                mask = 0x80;
            }
            out[i] = (current & mask) != 0;
            mask >>= 1;
        }
        return out;
    }

    // "All defined" byte, followed by a bit vector when it is zero.
    std::vector<bool> defined(std::size_t n) { return byte() != 0 ? std::vector<bool>(n, true) : bits(n); }

    void expect(std::uint8_t property)
    {
        if (byte() != property) {
            corrupt("unexpected header structure");
        }
    }

    void no_external()
    {
        if (byte() != 0) {
            throw Error(ErrorCode::Unsupported, "7z: external header data is not supported");
        }
    }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
};

struct Coder {
    std::uint64_t method = 0;
    std::vector<std::uint8_t> properties;
    std::size_t inputs = 1;
};

// One solid block: a graph of coders whose final output holds one or more files.
struct Folder {
    std::vector<Coder> coders;
    std::vector<std::pair<std::size_t, std::size_t>> bindings; // (coder input index, coder output index)
    std::vector<std::size_t> packed;                            // coder inputs fed by pack streams
    std::vector<std::uint64_t> unpack_sizes;                    // one per coder output
    std::optional<std::uint32_t> crc;
    std::size_t first_pack = 0;
    std::size_t substreams = 1;
    std::size_t main_coder = 0;

    std::uint64_t size() const { return unpack_sizes[main_coder]; }

    std::size_t first_input(std::size_t coder) const
    {
        std::size_t index = 0;
        for (std::size_t i = 0; i < coder; ++i) {
            index += coders[i].inputs;
        }
        return index;
    }

    std::optional<std::size_t> producer_of(std::size_t input) const
    {
        for (const auto& [in, out] : bindings) {
            if (in == input) {
                return out;
            }
        }
        return std::nullopt;
    }
};

struct Streams {
    std::vector<std::uint64_t> pack_sizes;
    std::vector<std::uint64_t> pack_offsets;
    std::vector<Folder> folders;
    std::vector<std::uint64_t> sizes; // per file stream
    std::vector<std::optional<std::uint32_t>> crcs;
};

struct Item {
    std::string name;
    bool has_stream = false;
    bool directory = false;
    bool anti = false;
    std::optional<Timestamp> mtime;
    std::optional<std::uint32_t> attributes;
    std::size_t folder = 0;
    std::uint64_t size = 0;
    std::optional<std::uint32_t> crc;
};

std::optional<std::uint64_t> liblzma_filter(std::uint64_t m)
{
    switch (m) {
    case method::Lzma:
        return LZMA_FILTER_LZMA1;
    case method::Lzma2:
        return LZMA_FILTER_LZMA2;
    case method::Delta:
        return LZMA_FILTER_DELTA;
    case method::X86:
        return LZMA_FILTER_X86;
    case method::PowerPc:
        return LZMA_FILTER_POWERPC;
    case method::Ia64:
        return LZMA_FILTER_IA64;
    case method::Arm:
        return LZMA_FILTER_ARM;
    case method::ArmThumb:
        return LZMA_FILTER_ARMTHUMB;
    case method::Sparc:
        return LZMA_FILTER_SPARC;
    case method::Arm64:
        return LZMA_FILTER_ARM64;
    case method::RiscV:
        return LZMA_FILTER_RISCV;
    default:
        return std::nullopt;
    }
}

const char* method_name(std::uint64_t m)
{
    switch (m) {
    case method::Copy:
        return "copy";
    case method::Lzma:
        return "lzma";
    case method::Lzma2:
        return "lzma2";
    case method::Ppmd:
        return "ppmd";
    case method::Bzip2:
        return "bzip2";
    case method::Deflate:
        return "deflate";
    case method::Deflate64:
        return "deflate64";
    case method::Zstd:
        return "zstd";
    case method::Bcj2:
        return "bcj2";
    case method::Aes:
        return "aes";
    case method::Delta:
        return "delta";
    case method::X86:
        return "bcj";
    case method::Arm64:
        return "arm64";
    default:
        return liblzma_filter(m) ? "bcj" : "unknown";
    }
}

class SevenZipReader final : public ArchiveReader {
public:
    SevenZipReader(io::SourcePtr source, Budget& budget) : source_(std::move(source)), budget_(budget)
    {
        read_archive();
    }

    bool next(Entry& entry) override
    {
        finish_current();
        while (++index_ < items_.size() && items_[index_].anti) {
        }
        if (index_ >= items_.size()) {
            return false;
        }
        const Item& item = items_[index_];
        entry = Entry{};
        entry.path = item.name;
        std::replace(entry.path.begin(), entry.path.end(), '\\', '/');
        entry.mtime = item.mtime;
        entry.type = item.directory ? EntryType::Directory : EntryType::File;
        if (item.attributes && (*item.attributes & kAttributeUnixExtension)) {
            const std::uint32_t unix_mode = *item.attributes >> 16;
            entry.mode = unix_mode & 07777;
            switch (unix_mode & 0170000) {
            case 0120000:
                entry.type = EntryType::Symlink;
                break;
            case 0040000:
                entry.type = EntryType::Directory;
                break;
            case 0100000:
            case 0:
                break;
            default:
                entry.type = EntryType::Special;
                break;
            }
        }
        if (item.has_stream) {
            entry.size = item.size;
            entry.crc32 = item.crc;
            entry.method = folder_method(streams_.folders[item.folder]);
            entry.encrypted = uses_encryption(streams_.folders[item.folder]);
            open_current(item);
        } else if (entry.type == EntryType::File) {
            entry.size = 0;
        }
        if (entry.type == EntryType::Symlink) {
            entry.link_target = read_link_target();
        }
        return true;
    }

    io::InputStream& data() override
    {
        const Item& item = items_[index_];
        if (!item.has_stream) {
            return empty_;
        }
        if (pending_error_) {
            std::rethrow_exception(pending_error_);
        }
        return *entry_stream_;
    }

    bool independent_entries() const override { return true; }

private:
    void read_archive()
    {
        std::uint8_t start[kSignatureHeaderSize];
        if (source_->size() < sizeof start) {
            corrupt("the file is too small to be a 7z archive");
        }
        source_->read_at(0, start, sizeof start);
        if (std::memcmp(start, kSignature, sizeof kSignature) != 0) {
            corrupt("bad signature");
        }
        if (start[6] != 0) {
            throw Error(ErrorCode::Unsupported, "7z: unsupported format version");
        }
        if (le32(start + 8) != crc32(0, start + 12, 20)) {
            throw Error(ErrorCode::Checksum, "7z: start header CRC mismatch");
        }
        const std::uint64_t offset = le64(start + 12);
        const std::uint64_t size = le64(start + 20);
        if (size == 0) {
            return; // empty archive
        }
        const std::uint64_t file_size = source_->size();
        if (offset > file_size - kSignatureHeaderSize || size > file_size - kSignatureHeaderSize - offset) {
            corrupt("truncated archive (header missing)");
        }
        if (size > kMaxHeaderSize) {
            throw Error(ErrorCode::LimitExceeded, "7z: the archive header is too large");
        }
        std::vector<std::uint8_t> header(static_cast<std::size_t>(size));
        source_->read_at(kSignatureHeaderSize + offset, header.data(), header.size());
        if (le32(start + 28) != crc32(0, header.data(), static_cast<uInt>(header.size()))) {
            throw Error(ErrorCode::Checksum, "7z: header CRC mismatch");
        }

        // The header itself is usually compressed ("encoded header"), sometimes more than once.
        for (int depth = 0; !header.empty() && header[0] == id::EncodedHeader; ++depth) {
            if (depth == 4) {
                corrupt("too many nested encoded headers");
            }
            header = decode_header(header);
        }
        HeaderReader reader(header.data(), header.size());
        reader.expect(id::Header);
        read_header(reader);
    }

    std::vector<std::uint8_t> decode_header(const std::vector<std::uint8_t>& encoded)
    {
        HeaderReader reader(encoded.data(), encoded.size());
        reader.expect(id::EncodedHeader);
        Streams streams;
        read_streams_info(reader, streams);
        if (streams.folders.empty()) {
            corrupt("encoded header without data");
        }
        const Folder& folder = streams.folders[0];
        if (folder.size() > kMaxHeaderSize) {
            throw Error(ErrorCode::LimitExceeded, "7z: the archive header is too large");
        }
        if (uses_encryption(folder)) {
            throw Error(ErrorCode::Unsupported, "7z: archives with encrypted file names are not supported yet");
        }
        std::vector<std::uint8_t> decoded(static_cast<std::size_t>(folder.size()));
        auto stream = build_folder(streams, 0);
        stream->read_exact(decoded.data(), decoded.size());
        if (folder.crc && *folder.crc != crc32(0, decoded.data(), static_cast<uInt>(decoded.size()))) {
            throw Error(ErrorCode::Checksum, "7z: header CRC mismatch");
        }
        return decoded;
    }

    void read_header(HeaderReader& r)
    {
        std::uint8_t type = r.byte();
        if (type == id::ArchiveProperties) {
            while (r.byte() != id::End) {
                r.take(r.number());
            }
            type = r.byte();
        }
        if (type == id::AdditionalStreamsInfo) {
            Streams ignored;
            read_streams_info(r, ignored);
            type = r.byte();
        }
        if (type == id::MainStreamsInfo) {
            read_streams_info(r, streams_);
            type = r.byte();
        }
        if (type == id::FilesInfo) {
            read_files(r);
            type = r.byte();
        }
        if (type != id::End) {
            corrupt("unexpected header structure");
        }
    }

    void read_streams_info(HeaderReader& r, Streams& s)
    {
        bool has_substreams = false;
        for (std::uint8_t type = r.byte(); type != id::End; type = r.byte()) {
            switch (type) {
            case id::PackInfo:
                read_pack_info(r, s);
                break;
            case id::UnpackInfo:
                read_unpack_info(r, s);
                break;
            case id::SubStreamsInfo:
                read_substreams(r, s);
                has_substreams = true;
                break;
            default:
                corrupt("unexpected header structure");
            }
        }
        std::size_t pack = 0;
        for (Folder& folder : s.folders) {
            folder.first_pack = pack;
            pack += folder.packed.size();
        }
        if (pack > s.pack_sizes.size()) {
            corrupt("missing packed streams");
        }
        if (!has_substreams) {
            for (const Folder& folder : s.folders) {
                s.sizes.push_back(folder.size());
                s.crcs.push_back(folder.crc);
            }
        }
    }

    void read_pack_info(HeaderReader& r, Streams& s)
    {
        const std::uint64_t position = r.number();
        const std::size_t count = r.count();
        s.pack_sizes.assign(count, 0);
        for (std::uint8_t type = r.byte(); type != id::End; type = r.byte()) {
            if (type == id::Size) {
                for (auto& size : s.pack_sizes) {
                    size = r.number();
                }
            } else if (type == id::Crc) {
                const auto defined = r.defined(count);
                for (bool d : defined) {
                    if (d) {
                        r.u32();
                    }
                }
            } else {
                corrupt("unexpected header structure");
            }
        }
        const std::uint64_t file_size = source_->size();
        std::uint64_t offset = kSignatureHeaderSize + position;
        if (position > file_size) {
            corrupt("packed data lies outside the archive");
        }
        for (const std::uint64_t size : s.pack_sizes) {
            if (size > file_size || offset > file_size - size) {
                corrupt("packed data lies outside the archive");
            }
            s.pack_offsets.push_back(offset);
            offset += size;
        }
    }

    void read_unpack_info(HeaderReader& r, Streams& s)
    {
        r.expect(id::Folder);
        const std::size_t count = r.count();
        r.no_external();
        s.folders.resize(count);
        for (Folder& folder : s.folders) {
            read_folder(r, folder);
        }
        r.expect(id::CodersUnpackSize);
        for (Folder& folder : s.folders) {
            for (std::size_t i = 0; i < folder.coders.size(); ++i) {
                folder.unpack_sizes.push_back(r.number());
            }
        }
        for (std::uint8_t type = r.byte(); type != id::End; type = r.byte()) {
            if (type != id::Crc) {
                corrupt("unexpected header structure");
            }
            const auto defined = r.defined(count);
            for (std::size_t i = 0; i < count; ++i) {
                if (defined[i]) {
                    s.folders[i].crc = r.u32();
                }
            }
        }
    }

    static void read_folder(HeaderReader& r, Folder& folder)
    {
        const std::size_t coder_count = r.count();
        if (coder_count == 0 || coder_count > kMaxCoders) {
            throw Error(ErrorCode::Unsupported, "7z: unsupported number of coders");
        }
        std::size_t total_inputs = 0;
        for (std::size_t i = 0; i < coder_count; ++i) {
            const std::uint8_t flags = r.byte();
            if (flags & 0x80) {
                throw Error(ErrorCode::Unsupported, "7z: alternative coder methods are not supported");
            }
            const std::size_t id_size = flags & 0x0F;
            if (id_size > 8) {
                throw Error(ErrorCode::Unsupported, "7z: unsupported method identifier");
            }
            Coder coder;
            const std::uint8_t* id_bytes = r.take(id_size);
            for (std::size_t k = 0; k < id_size; ++k) {
                coder.method = (coder.method << 8) | id_bytes[k];
            }
            if (flags & 0x10) {
                const std::uint64_t inputs = r.number();
                const std::uint64_t outputs = r.number();
                if (inputs == 0 || inputs > kMaxCoders || outputs != 1) {
                    throw Error(ErrorCode::Unsupported, "7z: unsupported coder layout");
                }
                coder.inputs = static_cast<std::size_t>(inputs);
            }
            if (flags & 0x20) {
                const std::size_t size = r.count();
                const std::uint8_t* props = r.take(size);
                coder.properties.assign(props, props + size);
            }
            total_inputs += coder.inputs;
            folder.coders.push_back(std::move(coder));
        }

        std::vector<bool> input_bound(total_inputs);
        std::vector<bool> output_bound(coder_count);
        for (std::size_t i = 0; i + 1 < coder_count; ++i) {
            const std::uint64_t in = r.number();
            const std::uint64_t out = r.number();
            if (in >= total_inputs || out >= coder_count || input_bound[in] || output_bound[out]) {
                corrupt("invalid coder binding");
            }
            input_bound[in] = output_bound[out] = true;
            folder.bindings.emplace_back(static_cast<std::size_t>(in), static_cast<std::size_t>(out));
        }
        const std::size_t packed_count = total_inputs - (coder_count - 1);
        if (packed_count == 1) {
            for (std::size_t i = 0; i < total_inputs; ++i) {
                if (!input_bound[i]) {
                    folder.packed.push_back(i);
                }
            }
        } else {
            for (std::size_t i = 0; i < packed_count; ++i) {
                const std::uint64_t in = r.number();
                if (in >= total_inputs || input_bound[in]) {
                    corrupt("invalid packed stream binding");
                }
                input_bound[in] = true;
                folder.packed.push_back(static_cast<std::size_t>(in));
            }
        }
        if (folder.packed.size() != packed_count) {
            corrupt("invalid packed stream binding");
        }
        for (std::size_t i = 0; i < coder_count; ++i) {
            if (!output_bound[i]) {
                folder.main_coder = i;
            }
        }
    }

    static void read_substreams(HeaderReader& r, Streams& s)
    {
        std::uint8_t type = r.byte();
        if (type == id::NumUnpackStream) {
            for (Folder& folder : s.folders) {
                folder.substreams = r.count();
            }
            type = r.byte();
        }
        for (const Folder& folder : s.folders) {
            if (folder.substreams == 0) {
                continue;
            }
            std::uint64_t used = 0;
            if (type == id::Size) {
                for (std::size_t i = 0; i + 1 < folder.substreams; ++i) {
                    const std::uint64_t size = r.number();
                    if (size > folder.size() - used) {
                        corrupt("file sizes exceed their solid block");
                    }
                    s.sizes.push_back(size);
                    used += size;
                }
            } else if (folder.substreams > 1) {
                corrupt("missing file sizes");
            }
            s.sizes.push_back(folder.size() - used);
        }
        if (type == id::Size) {
            type = r.byte();
        }

        // CRCs are listed only for streams whose CRC is not already the folder CRC.
        std::size_t missing = 0;
        for (const Folder& folder : s.folders) {
            if (folder.substreams == 1 && folder.crc) {
                s.crcs.push_back(folder.crc);
            } else {
                for (std::size_t i = 0; i < folder.substreams; ++i) {
                    s.crcs.emplace_back();
                    ++missing;
                }
            }
        }
        for (; type != id::End; type = r.byte()) {
            if (type != id::Crc) {
                corrupt("unexpected header structure");
            }
            const auto defined = r.defined(missing);
            std::size_t k = 0;
            std::size_t stream = 0;
            for (const Folder& folder : s.folders) {
                if (folder.substreams == 1 && folder.crc) {
                    ++stream;
                    continue;
                }
                for (std::size_t i = 0; i < folder.substreams; ++i, ++stream, ++k) {
                    if (defined[k]) {
                        s.crcs[stream] = r.u32();
                    }
                }
            }
        }
    }

    void read_files(HeaderReader& r)
    {
        const std::size_t count = r.count();
        const std::uint64_t max_entries = budget_.limits().max_entries;
        if (max_entries != 0 && count > max_entries) {
            throw Error(ErrorCode::LimitExceeded, "the archive has more than " + std::to_string(max_entries) +
                                                      " entries (limit: max_entries)");
        }
        items_.resize(count);
        std::vector<bool> empty_stream(count);
        std::vector<bool> empty_file;
        std::vector<bool> anti;
        std::size_t empty_count = 0;

        for (std::uint64_t type = r.number(); type != id::End; type = r.number()) {
            const std::uint64_t size = r.number();
            HeaderReader p(r.take(size), static_cast<std::size_t>(size));
            switch (type) {
            case id::EmptyStream:
                empty_stream = p.bits(count);
                empty_count = static_cast<std::size_t>(std::count(empty_stream.begin(), empty_stream.end(), true));
                break;
            case id::EmptyFile:
                empty_file = p.bits(empty_count);
                break;
            case id::Anti:
                anti = p.bits(empty_count);
                break;
            case id::Name:
                p.no_external();
                for (Item& item : items_) {
                    item.name = read_name(p);
                }
                break;
            case id::MTime: {
                const auto defined = p.defined(count);
                p.no_external();
                for (std::size_t i = 0; i < count; ++i) {
                    if (defined[i]) {
                        items_[i].mtime = from_filetime(p.u64());
                    }
                }
                break;
            }
            case id::WinAttributes: {
                const auto defined = p.defined(count);
                p.no_external();
                for (std::size_t i = 0; i < count; ++i) {
                    if (defined[i]) {
                        items_[i].attributes = p.u32();
                    }
                }
                break;
            }
            default:
                break; // creation and access times, comments, padding
            }
        }

        std::size_t stream = 0;
        std::size_t folder = 0;
        std::size_t in_folder = 0;
        std::size_t empty_index = 0;
        for (std::size_t i = 0; i < count; ++i) {
            Item& item = items_[i];
            item.has_stream = !empty_stream[i];
            if (!item.has_stream) {
                const bool is_file = empty_index < empty_file.size() && empty_file[empty_index];
                item.anti = empty_index < anti.size() && anti[empty_index];
                item.directory = !is_file;
                ++empty_index;
                continue;
            }
            while (folder < streams_.folders.size() && in_folder >= streams_.folders[folder].substreams) {
                ++folder;
                in_folder = 0;
            }
            if (folder >= streams_.folders.size() || stream >= streams_.sizes.size()) {
                corrupt("more files than data streams");
            }
            item.folder = folder;
            item.size = streams_.sizes[stream];
            item.crc = streams_.crcs[stream];
            ++stream;
            ++in_folder;
            if (item.attributes && (*item.attributes & kAttributeDirectory)) {
                item.directory = true;
            }
        }
    }

    static std::string read_name(HeaderReader& p)
    {
        std::vector<std::uint8_t> utf16;
        for (;;) {
            const std::uint8_t lo = p.byte();
            const std::uint8_t hi = p.byte();
            if (lo == 0 && hi == 0) {
                break;
            }
            utf16.push_back(lo);
            utf16.push_back(hi);
        }
        return text::utf16le_to_utf8(utf16.data(), utf16.size());
    }

    static bool uses_encryption(const Folder& folder)
    {
        return std::any_of(folder.coders.begin(), folder.coders.end(),
                           [](const Coder& c) { return c.method == method::Aes; });
    }

    static std::string folder_method(const Folder& folder)
    {
        std::string out;
        for (const Coder& coder : folder.coders) {
            if (!out.empty()) {
                out += '+';
            }
            out += method_name(coder.method);
        }
        return out;
    }

    // Builds the decoder for one folder: the output of its main coder.
    io::StreamPtr build_folder(const Streams& s, std::size_t index) const
    {
        const Folder& folder = s.folders[index];
        std::vector<bool> used(folder.coders.size());
        return build_coder(s, folder, folder.main_coder, used);
    }

    io::StreamPtr coder_input(const Streams& s, const Folder& folder, std::size_t input, std::vector<bool>& used) const
    {
        if (const auto producer = folder.producer_of(input)) {
            return build_coder(s, folder, *producer, used);
        }
        for (std::size_t j = 0; j < folder.packed.size(); ++j) {
            if (folder.packed[j] == input) {
                const std::size_t pack = folder.first_pack + j;
                return std::make_unique<io::SourceStream>(source_, s.pack_offsets[pack], s.pack_sizes[pack]);
            }
        }
        corrupt("unconnected coder input");
    }

    io::StreamPtr build_coder(const Streams& s, const Folder& folder, std::size_t index, std::vector<bool>& used) const
    {
        if (used[index]) {
            corrupt("coder cycle");
        }
        used[index] = true;
        const Coder& coder = folder.coders[index];
        const std::uint64_t memory = budget_.limits().max_memory;

        // Consecutive BCJ / Delta filters ending with LZMA or LZMA2 run as one liblzma chain.
        if (liblzma_filter(coder.method)) {
            std::vector<codec::LzmaFilter> chain;
            std::size_t current = index;
            for (;;) {
                const Coder& c = folder.coders[current];
                chain.push_back({*liblzma_filter(c.method), c.properties});
                const std::size_t input = folder.first_input(current);
                const bool is_codec = c.method == method::Lzma || c.method == method::Lzma2;
                const auto producer = folder.producer_of(input);
                if (!is_codec && producer && liblzma_filter(folder.coders[*producer].method)) {
                    if (used[*producer]) {
                        corrupt("coder cycle");
                    }
                    used[*producer] = true;
                    current = *producer;
                    continue;
                }
                return codec::decode_lzma_raw(coder_input(s, folder, input, used), chain,
                                              folder.unpack_sizes[current], memory);
            }
        }

        std::vector<io::StreamPtr> inputs;
        const std::size_t first = folder.first_input(index);
        for (std::size_t k = 0; k < coder.inputs; ++k) {
            inputs.push_back(coder_input(s, folder, first + k, used));
        }
        switch (coder.method) {
        case method::Copy:
            return std::move(inputs[0]);
        case method::Deflate:
            return codec::decode_deflate(std::move(inputs[0]));
        case method::Deflate64:
            return codec::decode_inflate(std::move(inputs[0]), true);
        case method::Bzip2:
            return codec::decode_bzip2(std::move(inputs[0]));
        case method::Zstd:
            return codec::decode_zstd(std::move(inputs[0]), memory);
        case method::Bcj2:
            return codec::decode_bcj2(std::move(inputs), folder.unpack_sizes[index]);
        case method::Aes:
            throw Error(ErrorCode::Unsupported, "7z: encrypted archives are not supported yet");
        case method::Ppmd:
            throw Error(ErrorCode::Unsupported, "7z: the PPMd method is not supported yet");
        default:
            throw Error(ErrorCode::Unsupported, "7z: unsupported compression method");
        }
    }

    void open_current(const Item& item)
    {
        entry_stream_.reset();
        try {
            if (item.folder != current_folder_ || !folder_stream_) {
                folder_stream_.reset();
                current_folder_ = item.folder;
                auto decoded = build_folder(streams_, item.folder);
                folder_stream_ = std::make_unique<GuardedStream>(
                    std::make_unique<BudgetStream>(std::move(decoded), budget_));
            }
            slice_ = std::make_unique<io::SliceStream>(*folder_stream_, item.size);
            entry_stream_ = std::make_unique<CheckedStream>(std::make_unique<SliceRef>(*slice_), item.size, item.crc);
        } catch (const Error&) {
            // The block cannot be decoded (unsupported method...): data() reports it for each of its files.
            folder_stream_.reset();
            slice_.reset();
            pending_error_ = std::current_exception();
        }
    }

    void finish_current()
    {
        pending_error_ = nullptr;
        if (!slice_) {
            return;
        }
        entry_stream_.reset();
        try {
            slice_->skip(slice_->remaining());
        } catch (const Error&) {
            // The guarded block stream now refuses every read: the next files of this block fail
            // instead of being decoded from a wrong position.
        }
        slice_.reset();
    }

    std::string read_link_target()
    {
        try {
            io::InputStream& stream = data();
            std::string target(kMaxLinkTarget + 1, '\0');
            target.resize(stream.read_up_to(reinterpret_cast<std::uint8_t*>(target.data()), target.size()));
            return target.size() > kMaxLinkTarget ? std::string() : text::to_utf8_or_latin1(target);
        } catch (const Error&) {
            return {};
        }
    }

    // Non-owning view, so that the slice can outlive the CheckedStream wrapped around it.
    class SliceRef final : public io::InputStream {
    public:
        explicit SliceRef(io::InputStream& target) : target_(target) {}
        std::size_t read(std::uint8_t* data, std::size_t size) override { return target_.read(data, size); }

    private:
        io::InputStream& target_;
    };

    io::SourcePtr source_;
    Budget& budget_;
    Streams streams_;
    std::vector<Item> items_;
    std::size_t index_ = static_cast<std::size_t>(-1);

    std::size_t current_folder_ = static_cast<std::size_t>(-1);
    io::StreamPtr folder_stream_;
    std::unique_ptr<io::SliceStream> slice_;
    io::StreamPtr entry_stream_;
    std::exception_ptr pending_error_;
    EmptyStream empty_;
};

} // namespace

std::unique_ptr<ArchiveReader> make_sevenzip_reader(io::SourcePtr source, Budget& budget)
{
    return std::make_unique<SevenZipReader>(std::move(source), budget);
}

} // namespace digzip::format
