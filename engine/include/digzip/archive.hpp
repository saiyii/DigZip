#pragma once

#include <digzip/entry.hpp>
#include <digzip/error.hpp>
#include <digzip/export.hpp>
#include <digzip/format.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
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

struct Progress {
    std::uint64_t entries = 0;      // entries reached so far
    std::uint64_t bytes = 0;        // uncompressed bytes decoded so far
    std::uint64_t archive_read = 0; // archive bytes read so far: archive_read / archive_size is the best
    std::uint64_t archive_size = 0; // estimate of the overall progress
    std::string current;            // path of the entry being processed
};

struct Options {
    Limits limits;

    // Entry selection, applied to the paths as stored in the archive. '*' matches any sequence of
    // characters (including '/'), '?' any single character. A pattern also matches everything below
    // a directory it matches. An entry is processed if it matches an include pattern (or there are
    // none) and no exclude pattern.
    std::vector<std::string> include;
    std::vector<std::string> exclude;

    // Called for each entry and about every 100 ms while data is decoded, from the calling thread.
    std::function<void(const Progress&)> on_progress;

    // When set to true (from any thread), the operation stops as soon as possible with an
    // ErrorCode::Cancelled issue. Files already completed are kept, the file in progress is removed.
    const std::atomic<bool>* cancel = nullptr;
};

// What to do when an output path already exists.
enum class Overwrite {
    Skip,    // keep the existing file and report a warning
    Replace, // replace it, once the new file is complete
    Rename,  // extract under a free name: "name (1).ext", "name (2).ext"...
};

struct ExtractOptions : Options {
    unsigned strip_components = 0; // leading path components to remove; shallower entries are skipped
    Overwrite overwrite = Overwrite::Skip;
};

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
