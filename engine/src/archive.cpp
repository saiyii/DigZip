#include "extract/extractor.hpp"
#include "filter.hpp"
#include "format/reader.hpp"
#include "io/stream.hpp"
#include "monitor.hpp"

#include <digzip/archive.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <new>

namespace digzip {

namespace {

using EntryHandler = std::function<void(const Entry&, format::ArchiveReader&, Report&)>;

void add_error(Report& report, ErrorCode code, const std::string& path, const std::string& message)
{
    report.issues.push_back({Severity::Error, code, path, message});
}

bool can_continue(const Error& error, const format::ArchiveReader& reader)
{
    switch (error.code()) {
    case ErrorCode::LimitExceeded:
    case ErrorCode::Cancelled:
        return false;
    case ErrorCode::Corrupt:
    case ErrorCode::Checksum:
        return reader.independent_entries();
    default:
        return true;
    }
}

bool was_cancelled(const Report& report)
{
    return std::any_of(report.issues.begin(), report.issues.end(),
                       [](const Issue& i) { return i.code == ErrorCode::Cancelled; });
}

struct OpenedFile {
    io::SourcePtr source;
    std::shared_ptr<std::atomic<std::uint64_t>> read;
};

OpenedFile open_counted(const std::filesystem::path& archive)
{
    auto read = std::make_shared<std::atomic<std::uint64_t>>(0);
    return {io::count_reads(io::open_file(archive), read), read};
}

// Opens the archive and calls `handle` for each entry, turning every failure into an issue.
// `handle` sees every entry; selection by include/exclude is up to it.
void process(const std::filesystem::path& archive, const Options& options, Report& report, const EntryHandler& handle)
{
    std::string current;
    try {
        const OpenedFile file = open_counted(archive);
        Monitor monitor(options, file.source->size(), file.read);
        format::OpenedArchive opened = format::open_archive(file.source, archive, monitor);
        report.format = opened.format;

        Entry entry;
        while (opened.reader->next(entry)) {
            current = entry.path;
            monitor.add_entry(entry.path);
            try {
                handle(entry, *opened.reader, report);
            } catch (const Error& e) {
                add_error(report, e.code(), entry.path, e.what());
                if (!can_continue(e, *opened.reader)) {
                    return;
                }
            }
            current.clear();
        }
        monitor.finish();
    } catch (const Error& e) {
        add_error(report, e.code(), current, e.what());
    } catch (const std::bad_alloc&) {
        add_error(report, ErrorCode::Io, current, "out of memory");
    } catch (const std::exception& e) {
        add_error(report, ErrorCode::Io, current, e.what());
    }
}

} // namespace

ArchiveFormat detect(const std::filesystem::path& archive)
{
    const io::SourcePtr source = io::open_file(archive);
    ArchiveFormat format = format::detect_format(*source);
    if (format.container == Container::Raw) {
        const Options defaults;
        Monitor monitor(defaults, source->size());
        format = format::open_archive(source, archive, monitor).format;
    }
    return format;
}

ArchiveInfo list(const std::filesystem::path& archive, const Options& options)
{
    const OpenedFile file = open_counted(archive);
    Monitor monitor(options, file.source->size(), file.read);
    format::OpenedArchive opened = format::open_archive(file.source, archive, monitor);

    ArchiveInfo info;
    info.format = opened.format;
    Entry entry;
    while (opened.reader->next(entry)) {
        monitor.add_entry(entry.path);
        if (is_selected(entry.path, options)) {
            info.entries.push_back(entry);
        }
    }
    monitor.finish();
    return info;
}

Report extract(const std::filesystem::path& archive, const std::filesystem::path& destination,
               const ExtractOptions& options)
{
    Report report;
    std::unique_ptr<extraction::Extractor> extractor;
    try {
        extractor = std::make_unique<extraction::Extractor>(destination, options);
    } catch (const Error& e) {
        add_error(report, e.code(), {}, e.what());
        return report;
    }

    process(archive, options, report, [&](const Entry& entry, format::ArchiveReader& reader, Report& r) {
        extractor->extract(entry, reader, r);
    });
    if (!was_cancelled(report)) {
        extractor->finish(report);
    }
    return report;
}

Report test(const std::filesystem::path& archive, const Options& options)
{
    Report report;
    const extraction::NameRules rules = extraction::host_rules(options.limits.max_path_depth);
    std::vector<std::uint8_t> buffer(1 << 20);

    process(archive, options, report, [&](const Entry& entry, format::ArchiveReader& reader, Report& r) {
        if (entry.type == EntryType::Special || !is_selected(entry.path, options)) {
            return;
        }
        const extraction::SafePath path = extraction::sanitize_path(entry.path, rules);
        switch (entry.type) {
        case EntryType::File: {
            io::InputStream& data = reader.data();
            for (std::size_t n; (n = data.read(buffer.data(), buffer.size())) > 0;) {
                r.bytes += n;
            }
            ++r.files;
            break;
        }
        case EntryType::Directory:
            ++r.directories;
            break;
        case EntryType::Symlink:
            extraction::resolve_link_target(path.components, entry.link_target);
            ++r.links;
            break;
        case EntryType::Hardlink:
            extraction::sanitize_path(entry.link_target, rules);
            ++r.links;
            break;
        case EntryType::Special:
            break;
        }
    });
    return report;
}

} // namespace digzip
