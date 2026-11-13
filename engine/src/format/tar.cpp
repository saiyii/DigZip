#include "format/reader.hpp"
#include "text.hpp"

#include <digzip/error.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <string>

namespace digzip::format {

namespace {

constexpr std::size_t kBlock = 512;
constexpr std::uint64_t kMaxMetadataSize = 1 << 20; // GNU long names and pax records

// Field offsets of a ustar header.
constexpr std::size_t kName = 0, kNameSize = 100;
constexpr std::size_t kMode = 100, kModeSize = 8;
constexpr std::size_t kSize = 124, kSizeSize = 12;
constexpr std::size_t kMtime = 136, kMtimeSize = 12;
constexpr std::size_t kChecksum = 148, kChecksumSize = 8;
constexpr std::size_t kType = 156;
constexpr std::size_t kLinkName = 157, kLinkNameSize = 100;
constexpr std::size_t kMagic = 257;
constexpr std::size_t kPrefix = 345, kPrefixSize = 155;

using PaxRecords = std::map<std::string, std::string>;

[[noreturn]] void corrupt(const std::string& what)
{
    throw Error(ErrorCode::Corrupt, "tar: " + what);
}

std::string field_string(const std::uint8_t* field, std::size_t size)
{
    const auto* end = std::find(field, field + size, std::uint8_t{0});
    return std::string(reinterpret_cast<const char*>(field), static_cast<std::size_t>(end - field));
}

// Octal ASCII, or GNU base-256 when the high bit of the first byte is set.
bool parse_number(const std::uint8_t* field, std::size_t size, std::int64_t& value)
{
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    if (field[0] & 0x80) {
        // Big-endian two's complement; bit 6 of the first byte is the sign.
        const bool negative = (field[0] & 0x40) != 0;
        const std::uint64_t sign_bits = negative ? 0x1FF : 0;
        std::uint64_t bits = negative ? ~std::uint64_t{0} : 0;
        bits = (bits << 6) | (field[0] & 0x3Fu);
        for (std::size_t i = 1; i < size; ++i) {
            if ((bits >> 55) != sign_bits) {
                return false;
            }
            bits = (bits << 8) | field[i];
        }
        value = static_cast<std::int64_t>(bits);
        return true;
    }

    std::size_t i = 0;
    while (i < size && (field[i] == ' ' || field[i] == 0)) {
        ++i;
    }
    std::int64_t result = 0;
    for (; i < size && field[i] >= '0' && field[i] <= '7'; ++i) {
        if (result > (kMax >> 3)) {
            return false;
        }
        result = result * 8 + (field[i] - '0');
    }
    for (; i < size; ++i) {
        if (field[i] != ' ' && field[i] != 0) {
            return false;
        }
    }
    value = result;
    return true;
}

std::int64_t number(const std::uint8_t* block, std::size_t offset, std::size_t size, const char* name)
{
    std::int64_t value = 0;
    if (!parse_number(block + offset, size, value)) {
        corrupt(std::string("invalid ") + name + " field");
    }
    return value;
}

bool is_zero_block(const std::uint8_t* block)
{
    return std::all_of(block, block + kBlock, [](std::uint8_t b) { return b == 0; });
}

bool is_posix_ustar(const std::uint8_t* block)
{
    return std::memcmp(block + kMagic, "ustar\0", 6) == 0;
}

// "<length> <key>=<value>\n" records (POSIX.1-2001). An empty value deletes the key.
void parse_pax(const std::string& data, PaxRecords& records)
{
    std::size_t pos = 0;
    while (pos < data.size() && data[pos] != '\0') {
        const std::size_t space = data.find(' ', pos);
        if (space == std::string::npos || space == pos || space - pos > 20) {
            corrupt("malformed pax record");
        }
        std::uint64_t length = 0;
        for (std::size_t i = pos; i < space; ++i) {
            if (data[i] < '0' || data[i] > '9') {
                corrupt("malformed pax record length");
            }
            length = length * 10 + static_cast<std::uint64_t>(data[i] - '0');
        }
        if (length <= space - pos + 1 || length > data.size() - pos || data[pos + length - 1] != '\n') {
            corrupt("malformed pax record");
        }
        const std::string record = data.substr(space + 1, pos + length - space - 2);
        const std::size_t equals = record.find('=');
        if (equals == std::string::npos) {
            corrupt("malformed pax record");
        }
        const std::string key = record.substr(0, equals);
        const std::string value = record.substr(equals + 1);
        if (value.empty()) {
            records.erase(key);
        } else {
            records[key] = value;
        }
        pos += length;
    }
}

// Pax time: "[-]seconds[.fraction]".
bool parse_pax_time(const std::string& value, Timestamp& out)
{
    std::size_t i = 0;
    const bool negative = !value.empty() && value[0] == '-';
    if (negative) {
        ++i;
    }
    std::int64_t seconds = 0;
    const std::size_t digits_start = i;
    for (; i < value.size() && value[i] >= '0' && value[i] <= '9'; ++i) {
        if (seconds > std::numeric_limits<std::int64_t>::max() / 10 - 1) {
            return false;
        }
        seconds = seconds * 10 + (value[i] - '0');
    }
    if (i == digits_start) {
        return false;
    }
    std::uint32_t nanoseconds = 0;
    if (i < value.size() && value[i] == '.') {
        ++i;
        std::uint32_t scale = 100'000'000;
        for (; i < value.size() && value[i] >= '0' && value[i] <= '9'; ++i) {
            nanoseconds += static_cast<std::uint32_t>(value[i] - '0') * scale;
            scale /= 10;
        }
    }
    if (i != value.size()) {
        return false;
    }
    if (negative) {
        seconds = -seconds;
        if (nanoseconds != 0) {
            seconds -= 1;
            nanoseconds = 1'000'000'000 - nanoseconds;
        }
    }
    out = {seconds, nanoseconds};
    return true;
}

bool parse_pax_size(const std::string& value, std::uint64_t& out)
{
    if (value.empty() || value.size() > 19) {
        return false;
    }
    std::uint64_t result = 0;
    for (const char c : value) {
        if (c < '0' || c > '9') {
            return false;
        }
        result = result * 10 + static_cast<std::uint64_t>(c - '0');
    }
    out = result;
    return true;
}

const std::string* pax_value(const PaxRecords& local, const PaxRecords& global, const std::string& key)
{
    if (const auto it = local.find(key); it != local.end()) {
        return &it->second;
    }
    if (const auto it = global.find(key); it != global.end()) {
        return &it->second;
    }
    return nullptr;
}

class UnsupportedStream final : public io::InputStream {
public:
    explicit UnsupportedStream(std::string reason) : reason_(std::move(reason)) {}

    std::size_t read(std::uint8_t*, std::size_t) override { throw Error(ErrorCode::Unsupported, reason_); }

private:
    std::string reason_;
};

class TarReader final : public ArchiveReader {
public:
    explicit TarReader(io::StreamPtr stream) : stream_(std::move(stream)) {}

    bool next(Entry& entry) override
    {
        finish_current();

        PaxRecords pax;
        std::string long_name;
        std::string long_link;
        bool pending_metadata = false;

        for (;;) {
            std::uint8_t block[kBlock];
            const std::size_t got = stream_->read_up_to(block, kBlock);
            if (got == 0 || (got == kBlock && is_zero_block(block))) {
                if (pending_metadata) {
                    corrupt("archive ends after an extended header");
                }
                return false;
            }
            if (got < kBlock) {
                corrupt("truncated header");
            }
            if (!is_tar_header(block)) {
                corrupt("invalid header checksum");
            }

            const char type = static_cast<char>(block[kType]);
            std::uint64_t size = header_size(block);

            switch (type) {
            case 'L':
                long_name = read_metadata(size);
                pending_metadata = true;
                continue;
            case 'K':
                long_link = read_metadata(size);
                pending_metadata = true;
                continue;
            case 'x':
                parse_pax(read_metadata(size), pax);
                pending_metadata = true;
                continue;
            case 'g':
                parse_pax(read_metadata(size), global_pax_);
                continue;
            case 'V':
                skip_data(size);
                continue;
            default:
                break;
            }

            if (const std::string* value = pax_value(pax, global_pax_, "size")) {
                if (!parse_pax_size(*value, size)) {
                    corrupt("invalid pax size");
                }
            }

            entry = Entry{};
            entry.path = entry_name(block, pax, long_name);
            entry.mode = static_cast<std::uint32_t>(number(block, kMode, kModeSize, "mode") & 07777);
            entry.mtime = entry_mtime(block, pax);
            entry.type = entry_type(type);
            if (entry.type == EntryType::File && !entry.path.empty() && entry.path.back() == '/') {
                entry.type = EntryType::Directory;
            }
            while (entry.path.size() > 1 && entry.path.back() == '/') {
                entry.path.pop_back();
            }
            if (entry.type == EntryType::Symlink || entry.type == EntryType::Hardlink) {
                entry.link_target = link_name(block, pax, long_link);
            }
            if (entry.type == EntryType::File) {
                entry.size = size;
            }

            data_ = std::make_unique<io::SliceStream>(*stream_, size);
            padding_ = (kBlock - size % kBlock) % kBlock;
            unsupported_.clear();
            if (type == 'S' || has_sparse_keys(pax)) {
                unsupported_ = "tar: sparse files are not supported";
            } else if (type == 'M') {
                unsupported_ = "tar: multi-volume archives are not supported";
            }
            return true;
        }
    }

    io::InputStream& data() override
    {
        if (!unsupported_.empty()) {
            unsupported_stream_ = std::make_unique<UnsupportedStream>(unsupported_);
            return *unsupported_stream_;
        }
        return *data_;
    }

    bool independent_entries() const override { return false; }

private:
    void finish_current()
    {
        if (!data_) {
            return;
        }
        const std::uint64_t remaining = data_->remaining();
        if (data_->skip(remaining) != remaining || stream_->skip(padding_) != padding_) {
            corrupt("unexpected end of archive");
        }
        data_.reset();
    }

    static std::uint64_t header_size(const std::uint8_t* block)
    {
        const std::int64_t size = number(block, kSize, kSizeSize, "size");
        if (size < 0) {
            corrupt("negative entry size");
        }
        return static_cast<std::uint64_t>(size);
    }

    void skip_data(std::uint64_t size)
    {
        const std::uint64_t padded = size + (kBlock - size % kBlock) % kBlock;
        if (stream_->skip(padded) != padded) {
            corrupt("unexpected end of archive");
        }
    }

    std::string read_metadata(std::uint64_t size)
    {
        if (size > kMaxMetadataSize) {
            corrupt("extended header is too large");
        }
        std::string data(static_cast<std::size_t>(size), '\0');
        stream_->read_exact(reinterpret_cast<std::uint8_t*>(data.data()), data.size());
        const std::uint64_t padding = (kBlock - size % kBlock) % kBlock;
        if (stream_->skip(padding) != padding) {
            corrupt("unexpected end of archive");
        }
        return data;
    }

    std::string entry_name(const std::uint8_t* block, const PaxRecords& pax, const std::string& long_name) const
    {
        if (const std::string* path = pax_value(pax, global_pax_, "path")) {
            return text::to_utf8_or_latin1(*path);
        }
        if (!long_name.empty()) {
            return text::to_utf8_or_latin1(field_string(reinterpret_cast<const std::uint8_t*>(long_name.data()), long_name.size()));
        }
        std::string name = field_string(block + kName, kNameSize);
        if (is_posix_ustar(block)) {
            const std::string prefix = field_string(block + kPrefix, kPrefixSize);
            if (!prefix.empty()) {
                name = prefix + "/" + name;
            }
        }
        return text::to_utf8_or_latin1(name);
    }

    std::string link_name(const std::uint8_t* block, const PaxRecords& pax, const std::string& long_link) const
    {
        if (const std::string* path = pax_value(pax, global_pax_, "linkpath")) {
            return text::to_utf8_or_latin1(*path);
        }
        if (!long_link.empty()) {
            return text::to_utf8_or_latin1(field_string(reinterpret_cast<const std::uint8_t*>(long_link.data()), long_link.size()));
        }
        return text::to_utf8_or_latin1(field_string(block + kLinkName, kLinkNameSize));
    }

    std::optional<Timestamp> entry_mtime(const std::uint8_t* block, const PaxRecords& pax) const
    {
        if (const std::string* value = pax_value(pax, global_pax_, "mtime")) {
            Timestamp t;
            if (parse_pax_time(*value, t)) {
                return t;
            }
        }
        return Timestamp{number(block, kMtime, kMtimeSize, "mtime"), 0};
    }

    static EntryType entry_type(char type)
    {
        switch (type) {
        case '1':
            return EntryType::Hardlink;
        case '2':
            return EntryType::Symlink;
        case '3':
        case '4':
        case '6':
            return EntryType::Special;
        case '5':
        case 'D':
            return EntryType::Directory;
        default:
            return EntryType::File; // POSIX: unknown types are read as regular files
        }
    }

    static bool has_sparse_keys(const PaxRecords& pax)
    {
        const auto it = pax.lower_bound("GNU.sparse.");
        return it != pax.end() && it->first.compare(0, 11, "GNU.sparse.") == 0;
    }

    io::StreamPtr stream_;
    PaxRecords global_pax_;
    std::unique_ptr<io::SliceStream> data_;
    std::uint64_t padding_ = 0;
    std::string unsupported_;
    std::unique_ptr<UnsupportedStream> unsupported_stream_;
};

} // namespace

bool is_tar_header(const std::uint8_t* block)
{
    if (is_zero_block(block)) {
        return false;
    }
    std::int64_t stored = 0;
    if (!parse_number(block + kChecksum, kChecksumSize, stored)) {
        return false;
    }
    std::int64_t unsigned_sum = 0;
    std::int64_t signed_sum = 0;
    for (std::size_t i = 0; i < kBlock; ++i) {
        const bool in_field = i >= kChecksum && i < kChecksum + kChecksumSize;
        const std::uint8_t byte = in_field ? std::uint8_t{' '} : block[i];
        unsigned_sum += byte;
        signed_sum += static_cast<signed char>(byte);
    }
    return stored == unsigned_sum || stored == signed_sum;
}

std::unique_ptr<ArchiveReader> make_tar_reader(io::StreamPtr stream)
{
    return std::make_unique<TarReader>(std::move(stream));
}

} // namespace digzip::format
