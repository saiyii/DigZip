#include <digzip/archive.hpp>

#include <algorithm>

namespace digzip {

const char* to_string(ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::Io:
        return "io";
    case ErrorCode::Corrupt:
        return "corrupt";
    case ErrorCode::Unsupported:
        return "unsupported";
    case ErrorCode::Checksum:
        return "checksum";
    case ErrorCode::Security:
        return "security";
    case ErrorCode::LimitExceeded:
        return "limit_exceeded";
    case ErrorCode::InvalidName:
        return "invalid_name";
    case ErrorCode::AlreadyExists:
        return "already_exists";
    case ErrorCode::InvalidArgument:
        return "invalid_argument";
    }
    return "unknown";
}

const char* to_string(EntryType type) noexcept
{
    switch (type) {
    case EntryType::File:
        return "file";
    case EntryType::Directory:
        return "directory";
    case EntryType::Symlink:
        return "symlink";
    case EntryType::Hardlink:
        return "hardlink";
    case EntryType::Special:
        return "special";
    }
    return "unknown";
}

std::string to_string(ArchiveFormat format)
{
    const char* compression = "";
    switch (format.compression) {
    case Compression::None:
        break;
    case Compression::Gzip:
        compression = "gz";
        break;
    case Compression::Bzip2:
        compression = "bz2";
        break;
    case Compression::Xz:
        compression = "xz";
        break;
    case Compression::Zstd:
        compression = "zst";
        break;
    }

    switch (format.container) {
    case Container::Zip:
        return "zip";
    case Container::SevenZip:
        return "7z";
    case Container::Rar:
        return "rar";
    case Container::Tar:
        return *compression ? std::string("tar.") + compression : "tar";
    case Container::Raw:
        return compression;
    case Container::Unknown:
        break;
    }
    return "unknown";
}

bool Report::ok() const noexcept
{
    return std::none_of(issues.begin(), issues.end(), [](const Issue& i) { return i.severity == Severity::Error; });
}

} // namespace digzip
