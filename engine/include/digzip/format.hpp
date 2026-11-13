#pragma once

#include <digzip/export.hpp>

#include <string>

namespace digzip {

enum class Container {
    Unknown,
    Zip,
    SevenZip,
    Rar,
    Tar,
    Raw, // a single compressed file, e.g. "notes.txt.gz"
};

// Compression wrapped around the whole container, as in "tar.gz".
enum class Compression {
    None,
    Gzip,
    Bzip2,
    Xz,
    Zstd,
};

struct ArchiveFormat {
    Container container = Container::Unknown;
    Compression compression = Compression::None;
};

// Short name such as "zip", "7z", "tar.xz" or "gz".
DIGZIP_EXPORT std::string to_string(ArchiveFormat format);

} // namespace digzip
