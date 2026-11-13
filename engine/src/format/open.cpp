#include "codec/decoders.hpp"
#include "format/reader.hpp"

#include <digzip/error.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

namespace digzip::format {

namespace {

constexpr std::size_t kSniffSize = 512;

class RawReader final : public ArchiveReader {
public:
    RawReader(io::StreamPtr stream, Entry entry) : stream_(std::move(stream)), entry_(std::move(entry)) {}

    bool next(Entry& entry) override
    {
        if (returned_) {
            return false;
        }
        returned_ = true;
        entry = entry_;
        return true;
    }

    io::InputStream& data() override { return *stream_; }
    bool independent_entries() const override { return false; }

private:
    io::StreamPtr stream_;
    Entry entry_;
    bool returned_ = false;
};

bool starts_with(const std::uint8_t* data, std::size_t size, std::initializer_list<std::uint8_t> magic)
{
    return size >= magic.size() && std::equal(magic.begin(), magic.end(), data);
}

Compression detect_compression(const std::uint8_t* head, std::size_t size)
{
    if (starts_with(head, size, {0x1F, 0x8B})) {
        return Compression::Gzip;
    }
    if (starts_with(head, size, {'B', 'Z', 'h'}) && size >= 4 && head[3] >= '1' && head[3] <= '9') {
        return Compression::Bzip2;
    }
    if (starts_with(head, size, {0xFD, '7', 'z', 'X', 'Z', 0x00})) {
        return Compression::Xz;
    }
    if (starts_with(head, size, {0x28, 0xB5, 0x2F, 0xFD})) {
        return Compression::Zstd;
    }
    return Compression::None;
}

Container detect_container(const std::uint8_t* head, std::size_t size)
{
    if (starts_with(head, size, {'P', 'K', 0x03, 0x04}) || starts_with(head, size, {'P', 'K', 0x05, 0x06}) ||
        starts_with(head, size, {'P', 'K', 0x07, 0x08})) {
        return Container::Zip;
    }
    if (starts_with(head, size, {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C})) {
        return Container::SevenZip;
    }
    if (starts_with(head, size, {'R', 'a', 'r', '!', 0x1A, 0x07})) {
        return Container::Rar;
    }
    if (size >= kSniffSize && is_tar_header(head)) {
        return Container::Tar;
    }
    return Container::Unknown;
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Name of the single file inside "name.ext.gz" and friends.
std::string raw_entry_name(const std::filesystem::path& archive, const std::string& stored_name)
{
    if (!stored_name.empty()) {
        const std::size_t slash = stored_name.find_last_of("/\\");
        std::string base = slash == std::string::npos ? stored_name : stored_name.substr(slash + 1);
        if (!base.empty() && base != "." && base != "..") {
            return base;
        }
    }
    const std::string file = archive.filename().u8string();
    const std::string lowered = lower(file);
    for (const char* ext : {".gz", ".gzip", ".bz2", ".bz", ".xz", ".zst", ".zstd"}) {
        const std::size_t n = std::strlen(ext);
        if (lowered.size() > n && lowered.compare(lowered.size() - n, n, ext) == 0) {
            return file.substr(0, file.size() - n);
        }
    }
    return file.empty() ? std::string("data") : file + ".out";
}

[[noreturn]] void unsupported_container(Container container)
{
    switch (container) {
    case Container::Rar:
        throw Error(ErrorCode::Unsupported, "RAR archives need the optional RAR module, which is not part of this build");
    default:
        throw Error(ErrorCode::Unsupported, "unrecognized archive format");
    }
}

} // namespace

ArchiveFormat detect_format(const io::Source& source)
{
    std::array<std::uint8_t, kSniffSize> head{};
    const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(head.size(), source.size()));
    source.read_at(0, head.data(), size);

    ArchiveFormat format;
    format.compression = detect_compression(head.data(), size);
    format.container = format.compression == Compression::None ? detect_container(head.data(), size) : Container::Raw;
    if (format.container == Container::Unknown && has_zip_directory(source)) {
        format.container = Container::Zip;
    }
    return format;
}

OpenedArchive open_archive(io::SourcePtr source, const std::filesystem::path& path, Budget& budget)
{
    const ArchiveFormat sniffed = detect_format(*source);
    const std::uint64_t size = source->size();
    auto whole = std::make_unique<io::SourceStream>(source, 0, size);

    if (sniffed.compression == Compression::None) {
        switch (sniffed.container) {
        case Container::Tar:
            return {sniffed, make_tar_reader(std::make_unique<BudgetStream>(std::move(whole), budget))};
        case Container::Zip:
            return {sniffed, make_zip_reader(source, budget)};
        case Container::SevenZip:
            return {sniffed, make_sevenzip_reader(source, budget)};
        default:
            unsupported_container(sniffed.container);
        }
    }

    Entry entry;
    entry.type = EntryType::File;
    io::StreamPtr decoded;
    switch (sniffed.compression) {
    case Compression::Gzip: {
        auto gzip = std::make_unique<codec::GzipStream>(std::move(whole));
        entry.path = raw_entry_name(path, gzip->header().name);
        entry.mtime = gzip->header().mtime;
        entry.method = "deflate";
        decoded = std::move(gzip);
        break;
    }
    case Compression::Bzip2:
        decoded = codec::decode_bzip2(std::move(whole));
        entry.method = "bzip2";
        break;
    case Compression::Xz:
        decoded = codec::decode_xz(std::move(whole), budget.limits().max_memory);
        entry.method = "xz";
        break;
    default:
        decoded = codec::decode_zstd(std::move(whole), budget.limits().max_memory);
        entry.method = "zstd";
        break;
    }
    if (entry.path.empty()) {
        entry.path = raw_entry_name(path, {});
    }

    auto counted = std::make_unique<BudgetStream>(std::move(decoded), budget);
    std::vector<std::uint8_t> prefix(kSniffSize);
    prefix.resize(counted->read_up_to(prefix.data(), prefix.size()));
    const bool is_tar = prefix.size() == kSniffSize && is_tar_header(prefix.data());
    auto stream = std::make_unique<io::PrefixedStream>(std::move(prefix), std::move(counted));

    if (is_tar) {
        return {{Container::Tar, sniffed.compression}, make_tar_reader(std::move(stream))};
    }
    entry.packed_size = size;
    return {sniffed, make_raw_reader(std::move(stream), std::move(entry))};
}

std::unique_ptr<ArchiveReader> make_raw_reader(io::StreamPtr stream, Entry entry)
{
    return std::make_unique<RawReader>(std::move(stream), std::move(entry));
}

} // namespace digzip::format
