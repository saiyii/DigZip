#include "extract/extractor.hpp"
#include "format/reader.hpp"
#include "io/stream.hpp"
#include "limits.hpp"

#include <digzip/archive.hpp>

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
        return false;
    case ErrorCode::Corrupt:
    case ErrorCode::Checksum:
        return reader.independent_entries();
    default:
        return true;
    }
}

// Opens the archive and calls `handle` for each entry, turning every failure into an issue.
void process(const std::filesystem::path& archive, const Limits& limits, Report& report, const EntryHandler& handle)
{
    std::string current;
    try {
        const io::SourcePtr source = io::open_file(archive);
        Budget budget(limits, source->size());
        format::OpenedArchive opened = format::open_archive(source, archive, budget);
        report.format = opened.format;

        Entry entry;
        while (opened.reader->next(entry)) {
            current = entry.path;
            budget.add_entry();
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
        Budget budget(Limits{}, source->size());
        format = format::open_archive(source, archive, budget).format;
    }
    return format;
}

ArchiveInfo list(const std::filesystem::path& archive, const Options& options)
{
    const io::SourcePtr source = io::open_file(archive);
    Budget budget(options.limits, source->size());
    format::OpenedArchive opened = format::open_archive(source, archive, budget);

    ArchiveInfo info;
    info.format = opened.format;
    Entry entry;
    while (opened.reader->next(entry)) {
        budget.add_entry();
        info.entries.push_back(entry);
    }
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

    process(archive, options.limits, report, [&](const Entry& entry, format::ArchiveReader& reader, Report& r) {
        extractor->extract(entry, reader, r);
    });
    extractor->finish(report);
    return report;
}

Report test(const std::filesystem::path& archive, const Options& options)
{
    Report report;
    const extraction::NameRules rules = extraction::host_rules(options.limits.max_path_depth);
    std::vector<std::uint8_t> buffer(1 << 20);

    process(archive, options.limits, report, [&](const Entry& entry, format::ArchiveReader& reader, Report& r) {
        if (entry.type == EntryType::Special) {
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
