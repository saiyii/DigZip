#include "extract/extractor.hpp"
#include "extract/output_file.hpp"
#include "filter.hpp"

#include <algorithm>
#include <random>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace digzip::extraction {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kCopyBufferSize = 1 << 20;
constexpr int kMaxRenameAttempts = 10'000;

[[noreturn]] void throw_fs(ErrorCode code, const std::string& what, const std::error_code& ec)
{
    throw Error(code, what + ": " + ec.message());
}

void warn(Report& report, ErrorCode code, const std::string& path, const std::string& message)
{
    report.issues.push_back({Severity::Warning, code, path, message});
}

fs::path native_link_target(const std::string& target)
{
    fs::path path = fs::u8path(target);
    path.make_preferred();
    return path;
}

// "name.ext" -> "name (1).ext", "name (2).ext"... first one that does not exist.
fs::path free_name(const fs::path& target)
{
    const std::string stem = target.stem().u8string();
    const std::string extension = target.extension().u8string();
    for (int n = 1; n <= kMaxRenameAttempts; ++n) {
        const fs::path candidate =
            target.parent_path() / fs::u8path(stem + " (" + std::to_string(n) + ")" + extension);
        std::error_code ec;
        if (fs::symlink_status(candidate, ec).type() == fs::file_type::not_found) {
            return candidate;
        }
    }
    throw Error(ErrorCode::AlreadyExists, "no free name left to rename the entry");
}

} // namespace

Extractor::Extractor(const fs::path& destination, const ExtractOptions& options)
    : options_(options), rules_(host_rules(options.limits.max_path_depth)), buffer_(kCopyBufferSize)
{
    std::error_code ec;
    root_ = long_path(destination);
    fs::create_directories(root_, ec);
    if (!fs::is_directory(root_)) {
        const std::string reason = ec ? ec.message() : "it is not a directory";
        throw Error(ErrorCode::InvalidArgument, "cannot extract into " + destination.u8string() + ": " + reason);
    }
    temp_counter_ = std::random_device{}();
}

std::optional<SafePath> Extractor::place(std::string_view archive_path) const
{
    SafePath path = sanitize_path(archive_path, rules_);
    const std::size_t strip = options_.strip_components;
    if (strip == 0) {
        return path;
    }
    if (path.components.size() <= strip) {
        return std::nullopt;
    }
    path.components.erase(path.components.begin(), path.components.begin() + static_cast<std::ptrdiff_t>(strip));
    return path;
}

fs::path Extractor::path_of(const std::vector<std::string>& components, std::size_t count) const
{
    fs::path path = root_;
    for (std::size_t i = 0; i < count; ++i) {
        path /= fs::u8path(components[i]);
    }
    return path;
}

void Extractor::make_directories(const std::vector<std::string>& components, std::size_t count)
{
    fs::path path = root_;
    std::string key;
    for (std::size_t i = 0; i < count; ++i) {
        path /= fs::u8path(components[i]);
        key += '/';
        key += components[i];
        if (directories_.count(key) != 0) {
            continue;
        }
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(path, ec);
        switch (status.type()) {
        case fs::file_type::not_found:
            if (!fs::create_directory(path, ec) && ec) {
                throw_fs(ErrorCode::Io, "cannot create the directory '" + key.substr(1) + "'", ec);
            }
            break;
        case fs::file_type::directory:
            break;
        case fs::file_type::regular:
            throw Error(ErrorCode::AlreadyExists, "'" + key.substr(1) + "' exists and is not a directory");
        case fs::file_type::none:
            throw_fs(ErrorCode::Io, "cannot inspect '" + key.substr(1) + "'", ec);
        default:
            // Symbolic links, junctions and special files are never traversed.
            throw Error(ErrorCode::Security, "'" + key.substr(1) + "' is a link or special file; refusing to write through it");
        }
        directories_.insert(key);
    }
}

std::optional<fs::path> Extractor::claim(const fs::path& target, const std::string& entry_path, Report& report)
{
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(target, ec);
    if (status.type() == fs::file_type::not_found) {
        return target;
    }
    if (status.type() == fs::file_type::none) {
        throw_fs(ErrorCode::Io, "cannot inspect the output path", ec);
    }
    switch (options_.overwrite) {
    case Overwrite::Skip:
        warn(report, ErrorCode::AlreadyExists, entry_path, "already exists, skipped");
        ++report.skipped;
        return std::nullopt;
    case Overwrite::Replace:
        if (status.type() == fs::file_type::directory) {
            throw Error(ErrorCode::AlreadyExists, "a directory with this name already exists");
        }
        return target;
    case Overwrite::Rename: {
        const fs::path renamed = free_name(target);
        warn(report, ErrorCode::AlreadyExists, entry_path,
             "already exists, extracted as '" + renamed.filename().u8string() + "'");
        return renamed;
    }
    }
    return std::nullopt;
}

void Extractor::extract(const Entry& entry, format::ArchiveReader& reader, Report& report)
{
    const bool selected = is_selected(entry.path, options_);
    if (entry.type == EntryType::File) {
        // Remember every file, so that a hard link to one that was not extracted is reported plainly.
        try {
            if (const auto path = place(entry.path)) {
                seen_files_.insert(join(path->components, path->components.size()));
            }
        } catch (const Error&) {
            if (selected) {
                throw;
            }
        }
    }
    if (!selected) {
        return;
    }
    if (entry.type == EntryType::Special) {
        warn(report, ErrorCode::Security, entry.path, "devices, FIFOs and sockets are never extracted");
        ++report.skipped;
        return;
    }

    const std::optional<SafePath> path = place(entry.path);
    if (!path) {
        return; // removed by strip_components
    }
    if (path->components.empty()) {
        if (entry.type == EntryType::Directory) {
            return; // "./" or "/" in the archive: the destination itself
        }
        throw Error(ErrorCode::InvalidName, "the entry has an empty name");
    }
    if (path->renamed) {
        warn(report, ErrorCode::InvalidName, entry.path,
             "renamed to '" + join(path->components, path->components.size()) + "' to be valid on this system");
    }

    switch (entry.type) {
    case EntryType::Directory:
        extract_directory(entry, *path, report);
        break;
    case EntryType::File:
        extract_file(entry, *path, reader, report);
        break;
    case EntryType::Hardlink:
        extract_hardlink(entry, *path, report);
        break;
    case EntryType::Symlink:
        // Created at the end, so that no later entry can be written through a link from the archive.
        links_.push_back({entry.path, path->components, resolve_link_target(path->components, entry.link_target),
                          entry.link_target});
        break;
    case EntryType::Special:
        break;
    }
}

void Extractor::extract_directory(const Entry& entry, const SafePath& path, Report& report)
{
    make_directories(path.components, path.components.size());
    ++report.directories;
    if (entry.mtime) {
        directory_times_.push_back({path_of(path.components, path.components.size()), *entry.mtime,
                                    path.components.size()});
    }
}

void Extractor::extract_file(const Entry& entry, const SafePath& path, format::ArchiveReader& reader, Report& report)
{
    const std::size_t depth = path.components.size();
    make_directories(path.components, depth - 1);
    const std::optional<fs::path> target = claim(path_of(path.components, depth), entry.path, report);
    if (!target) {
        return;
    }

    // Write to a temporary name, then rename: a failed entry never leaves a partial file behind,
    // and an existing file is only replaced once the new one is complete.
    const fs::path directory = target->parent_path();
    fs::path temp;
    std::optional<OutputFile> out;
    for (int attempt = 0; !out; ++attempt) {
        temp = directory / (".digzip-" + std::to_string(temp_counter_++) + ".tmp");
        try {
            out.emplace(temp, entry.mode);
        } catch (const Error& e) {
            if (e.code() != ErrorCode::AlreadyExists || attempt == 100) {
                throw;
            }
        }
    }

    std::uint64_t written = 0;
    try {
        io::InputStream& data = reader.data();
        for (std::size_t n; (n = data.read(buffer_.data(), buffer_.size())) > 0;) {
            out->write(buffer_.data(), n);
            written += n;
        }
        if (entry.mtime) {
            out->set_mtime(*entry.mtime);
        }
        out->close();
        std::error_code ec;
        fs::rename(temp, *target, ec);
        if (ec) {
            throw_fs(ErrorCode::Io, "cannot move the extracted file into place", ec);
        }
    } catch (...) {
        out.reset();
        std::error_code ignored;
        fs::remove(temp, ignored);
        throw;
    }

    files_[join(path.components, depth)] = *target;
    ++report.files;
    report.bytes += written;
}

void Extractor::extract_hardlink(const Entry& entry, const SafePath& path, Report& report)
{
    const std::optional<SafePath> target = place(entry.link_target);
    const std::string target_key = target ? join(target->components, target->components.size()) : std::string();
    const auto source = files_.find(target_key);
    if (source == files_.end()) {
        if (!target || seen_files_.count(target_key) != 0) {
            warn(report, ErrorCode::AlreadyExists, entry.path, "its target was not extracted, so the link is skipped");
            ++report.skipped;
            return;
        }
        throw Error(ErrorCode::Security,
                    "the hard link target '" + entry.link_target + "' is not a file from this archive");
    }

    const std::size_t depth = path.components.size();
    make_directories(path.components, depth - 1);
    const std::optional<fs::path> link = claim(path_of(path.components, depth), entry.path, report);
    if (!link) {
        return;
    }
    std::error_code ec;
    fs::remove(*link, ec); // only exists when replacing
    ec.clear();
    fs::create_hard_link(source->second, *link, ec);
    if (ec) {
        // File systems without hard links (FAT, some network shares) get a copy.
        ec.clear();
        fs::copy_file(source->second, *link, ec);
        if (ec) {
            throw_fs(ErrorCode::Io, "cannot create the hard link", ec);
        }
    }
    files_[join(path.components, depth)] = *link;
    ++report.links;
}

void Extractor::create_symlink(const PendingLink& link, Report& report)
{
    const std::size_t depth = link.components.size();
    make_directories(link.components, depth - 1);
    const std::optional<fs::path> path = claim(path_of(link.components, depth), link.entry_path, report);
    if (!path) {
        return;
    }

    std::error_code ec;
    fs::remove(*path, ec); // only exists when replacing
    ec.clear();
    const fs::path target = path_of(link.resolved_target, link.resolved_target.size());
    if (fs::is_directory(target, ec)) {
        fs::create_directory_symlink(native_link_target(link.target), *path, ec);
    } else {
        fs::create_symlink(native_link_target(link.target), *path, ec);
    }
    if (ec) {
#ifdef _WIN32
        if (ec.value() == ERROR_PRIVILEGE_NOT_HELD) {
            // Without the privilege, a link to a file from this archive becomes a copy of it.
            ec.clear();
            const auto source = files_.find(join(link.resolved_target, link.resolved_target.size()));
            if (source != files_.end() && fs::copy_file(source->second, *path, ec)) {
                warn(report, ErrorCode::Unsupported, link.entry_path,
                     "copied instead of linked: symbolic links need Developer Mode or administrator rights on Windows");
                files_[join(link.components, depth)] = *path;
                ++report.files;
                return;
            }
            warn(report, ErrorCode::Unsupported, link.entry_path,
                 "symbolic links need Developer Mode or administrator rights on Windows; link not created");
            ++report.skipped;
            return;
        }
#endif
        throw_fs(ErrorCode::Io, "cannot create the symbolic link", ec);
    }
    ++report.links;
}

void Extractor::finish(Report& report)
{
    for (const PendingLink& link : links_) {
        try {
            create_symlink(link, report);
        } catch (const Error& e) {
            report.issues.push_back({Severity::Error, e.code(), link.entry_path, e.what()});
        }
    }
    links_.clear();

    // Deepest first, so that setting a child's time does not change its parent afterwards.
    std::stable_sort(directory_times_.begin(), directory_times_.end(),
                     [](const DirectoryTime& a, const DirectoryTime& b) { return a.depth > b.depth; });
    for (const DirectoryTime& entry : directory_times_) {
        set_directory_mtime(entry.path, entry.mtime);
    }
    directory_times_.clear();
}

} // namespace digzip::extraction
