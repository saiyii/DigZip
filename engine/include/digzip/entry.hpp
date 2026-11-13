#pragma once

#include <digzip/export.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace digzip {

// Point in time, UTC, relative to the Unix epoch.
struct Timestamp {
    std::int64_t seconds = 0;
    std::uint32_t nanoseconds = 0;
};

enum class EntryType {
    File,
    Directory,
    Symlink,
    Hardlink,
    Special, // device, FIFO or socket: never extracted
};

DIGZIP_EXPORT const char* to_string(EntryType type) noexcept;

struct Entry {
    std::string path; // UTF-8, '/' separated, exactly as named in the archive (not sanitized)
    EntryType type = EntryType::File;
    std::optional<std::uint64_t> size;        // uncompressed size, when known before decoding
    std::optional<std::uint64_t> packed_size; // compressed size, when the format stores it per entry
    std::optional<Timestamp> mtime;
    std::optional<std::uint32_t> mode; // POSIX permission bits
    std::optional<std::uint32_t> crc32;
    std::string link_target; // for symbolic and hard links, as stored in the archive
    std::string method;      // compression method name, e.g. "deflate"; empty when not applicable
    bool encrypted = false;
};

} // namespace digzip
