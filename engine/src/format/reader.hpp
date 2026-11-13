#pragma once

#include "io/stream.hpp"
#include "limits.hpp"

#include <digzip/entry.hpp>
#include <digzip/format.hpp>

#include <filesystem>
#include <memory>

namespace digzip::format {

// Iterates over the entries of one archive.
class ArchiveReader {
public:
    ArchiveReader() = default;
    ArchiveReader(const ArchiveReader&) = delete;
    ArchiveReader& operator=(const ArchiveReader&) = delete;
    virtual ~ArchiveReader() = default;

    // Moves to the next entry. Returns false after the last one.
    virtual bool next(Entry& entry) = 0;

    // Content of the current entry, valid until the next call to next(). Sizes and checksums
    // are verified when the stream is read to its end; a mismatch throws Error.
    virtual io::InputStream& data() = 0;

    // True when corrupt data inside one entry still lets the reader move on to the next one.
    virtual bool independent_entries() const = 0;
};

struct OpenedArchive {
    ArchiveFormat format;
    std::unique_ptr<ArchiveReader> reader;
};

// Detects the format from the content. `path` only provides a file name for formats that do not
// store one (a single compressed file).
OpenedArchive open_archive(io::SourcePtr source, const std::filesystem::path& path, Budget& budget);

ArchiveFormat detect_format(const io::Source& source);

// Readers for each container.
std::unique_ptr<ArchiveReader> make_tar_reader(io::StreamPtr stream);
std::unique_ptr<ArchiveReader> make_raw_reader(io::StreamPtr stream, Entry entry);

// True if `block` (512 bytes) is a valid tar header.
bool is_tar_header(const std::uint8_t* block);

} // namespace digzip::format
