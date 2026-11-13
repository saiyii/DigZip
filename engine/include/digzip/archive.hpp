#pragma once

#include <digzip/entry.hpp>
#include <digzip/error.hpp>
#include <digzip/export.hpp>
#include <digzip/format.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace digzip {

// Anti-bomb guards. A value of 0 disables the corresponding limit.
struct Limits {
    std::uint64_t max_total_size = 64ull << 30;   // uncompressed bytes for the whole archive
    std::uint64_t max_entries = 1'000'000;
    std::uint64_t max_ratio = 1000;               // uncompressed bytes per archive byte
    std::uint64_t ratio_threshold = 64ull << 20;  // the ratio is only enforced past this many bytes
    std::uint32_t max_path_depth = 256;           // directory levels in one entry path
    std::uint64_t max_memory = 1ull << 30;        // memory a single decoder may allocate
};

struct Options {
    Limits limits;
};

struct ExtractOptions : Options {};

enum class Severity {
    Warning, // the operation went on; the result may differ from the archive (skipped or renamed entry)
    Error,   // an entry, or the whole archive, could not be processed
};

struct Issue {
    Severity severity = Severity::Error;
    ErrorCode code = ErrorCode::Io;
    std::string path; // entry path as stored in the archive; empty for archive-level problems
    std::string message;
};

struct Report {
    ArchiveFormat format;
    std::uint64_t files = 0;
    std::uint64_t directories = 0;
    std::uint64_t links = 0;
    std::uint64_t skipped = 0;
    std::uint64_t bytes = 0; // uncompressed bytes written (extract) or verified (test)
    std::vector<Issue> issues;

    bool ok() const noexcept;
};

struct ArchiveInfo {
    ArchiveFormat format;
    std::vector<Entry> entries;
};

// Identifies the archive format from its content. Throws Error.
DIGZIP_EXPORT ArchiveFormat detect(const std::filesystem::path& archive);

// Lists the entries without extracting them. Throws Error.
DIGZIP_EXPORT ArchiveInfo list(const std::filesystem::path& archive, const Options& options = {});

// Extracts into `destination`, created if needed. Never throws for archive or file system
// problems: they are reported in Report::issues.
DIGZIP_EXPORT Report extract(const std::filesystem::path& archive,
                             const std::filesystem::path& destination,
                             const ExtractOptions& options = {});

// Decodes every entry and verifies sizes and checksums without writing anything.
DIGZIP_EXPORT Report test(const std::filesystem::path& archive, const Options& options = {});

} // namespace digzip
