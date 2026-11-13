#pragma once

#include "extract/path_rules.hpp"
#include "format/reader.hpp"

#include <digzip/archive.hpp>

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace digzip::extraction {

// Writes entries below one destination directory, refusing anything that would land outside it.
class Extractor {
public:
    // Creates the destination if needed. Throws Error(InvalidArgument) if it is not a directory.
    Extractor(const std::filesystem::path& destination, const ExtractOptions& options);

    // Throws Error for problems limited to this entry; warnings go to `report`.
    void extract(const Entry& entry, format::ArchiveReader& reader, Report& report);

    // Creates the deferred symbolic links and sets directory times.
    void finish(Report& report);

private:
    struct PendingLink {
        std::string entry_path;
        std::vector<std::string> components;
        std::vector<std::string> resolved_target;
        std::string target;
    };

    struct DirectoryTime {
        std::filesystem::path path;
        Timestamp mtime;
        std::size_t depth;
    };

    std::filesystem::path path_of(const std::vector<std::string>& components, std::size_t count) const;
    void make_directories(const std::vector<std::string>& components, std::size_t count);
    bool claim(const std::filesystem::path& target, const std::string& entry_path, Report& report);

    void extract_directory(const Entry& entry, const SafePath& path, Report& report);
    void extract_file(const Entry& entry, const SafePath& path, format::ArchiveReader& reader, Report& report);
    void extract_hardlink(const Entry& entry, const SafePath& path, Report& report);
    void create_symlink(const PendingLink& link, Report& report);

    std::filesystem::path root_;
    NameRules rules_;
    std::set<std::string> directories_; // created or verified, relative to the root
    std::set<std::string> files_;       // regular files written by this extraction
    std::set<std::string> skipped_files_; // files left alone because the path existed
    std::vector<PendingLink> links_;
    std::vector<DirectoryTime> directory_times_;
    std::vector<std::uint8_t> buffer_;
    std::uint64_t temp_counter_;
};

} // namespace digzip::extraction
