#include "output.hpp"
#include "i18n.hpp"
#include "json.hpp"
#include "platform.hpp"

#include <digzip/version.hpp>

#include <algorithm>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace cli {

namespace {

// Civil date from days since 1970-01-01 (Howard Hinnant's algorithm).
std::string format_time(const digzip::Timestamp& t)
{
    const std::int64_t days = t.seconds >= 0 ? t.seconds / 86400 : (t.seconds - 86399) / 86400;
    const std::int64_t seconds_of_day = t.seconds - days * 86400;
    const std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const std::int64_t day = doy - (153 * mp + 2) / 5 + 1;
    const std::int64_t month = mp < 10 ? mp + 3 : mp - 9;
    const std::int64_t year = yoe + era * 400 + (month <= 2);
    char text[128];
    std::snprintf(text, sizeof text, "%04lld-%02lld-%02lld %02lld:%02lld:%02lld", static_cast<long long>(year),
                  static_cast<long long>(month), static_cast<long long>(day),
                  static_cast<long long>(seconds_of_day / 3600), static_cast<long long>(seconds_of_day / 60 % 60),
                  static_cast<long long>(seconds_of_day % 60));
    return text;
}

std::string format_size(std::uint64_t bytes)
{
    static const char* const units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024 && unit + 1 < sizeof units / sizeof units[0]) {
        value /= 1024;
        ++unit;
    }
    if (unit == 0) {
        return std::to_string(bytes) + " " + tr("B");
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << value;
    std::string number = out.str();
    if (language() == Language::French) {
        std::replace(number.begin(), number.end(), '.', ',');
    }
    return number + " " + tr(units[unit]);
}

// Cuts UTF-8 text to at most `bytes` bytes without splitting a character.
std::string truncate_utf8(const std::string& text, std::size_t bytes)
{
    if (text.size() <= bytes) {
        return text;
    }
    std::size_t end = bytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    return text.substr(0, end);
}

void write_entry_json(JsonWriter& json, const digzip::Entry& e)
{
    json.begin_object();
    json.key("path").value(e.path);
    json.key("type").value(digzip::to_string(e.type));
    if (e.size) {
        json.key("size").value(*e.size);
    }
    if (e.packed_size) {
        json.key("packed_size").value(*e.packed_size);
    }
    if (e.mtime) {
        json.key("mtime").value(e.mtime->seconds);
        json.key("mtime_utc").value(format_time(*e.mtime));
    }
    if (e.mode) {
        json.key("mode").value(static_cast<std::uint64_t>(*e.mode));
    }
    if (e.crc32) {
        char crc[9];
        std::snprintf(crc, sizeof crc, "%08x", static_cast<unsigned>(*e.crc32));
        json.key("crc32").value(crc);
    }
    if (!e.method.empty()) {
        json.key("method").value(e.method);
    }
    json.key("encrypted").value(e.encrypted);
    if (e.type == digzip::EntryType::Symlink || e.type == digzip::EntryType::Hardlink) {
        json.key("link_target").value(e.link_target);
    }
    json.end_object();
}

} // namespace

int exit_code(digzip::ErrorCode code)
{
    using digzip::ErrorCode;
    switch (code) {
    case ErrorCode::Cancelled:
        return kInterrupted;
    case ErrorCode::PasswordRequired:
    case ErrorCode::WrongPassword:
        return kPassword;
    case ErrorCode::Security:
    case ErrorCode::LimitExceeded:
        return kRefused;
    case ErrorCode::Corrupt:
    case ErrorCode::Checksum:
    case ErrorCode::Unsupported:
        return kDataError;
    case ErrorCode::Io:
    case ErrorCode::InvalidName:
    case ErrorCode::AlreadyExists:
    case ErrorCode::InvalidArgument:
        return kIoError;
    }
    return kIoError;
}

// The most significant problem decides: interruption, then passwords, security, data, I/O.
int exit_code(const digzip::Report& report)
{
    static const int priority[] = {kInterrupted, kPassword, kRefused, kDataError, kIoError};
    int result = kOk;
    for (const digzip::Issue& issue : report.issues) {
        if (issue.severity == digzip::Severity::Warning) {
            if (result == kOk) {
                result = kWarnings;
            }
            continue;
        }
        const int code = exit_code(issue.code);
        const auto rank = [&](int c) {
            return std::find(std::begin(priority), std::end(priority), c) - std::begin(priority);
        };
        if (result == kOk || result == kWarnings || rank(code) < rank(result)) {
            result = code;
        }
    }
    return result;
}

void print_version(std::ostream& out, bool json)
{
    const digzip::VersionInfo info = digzip::version_info();
    if (json) {
        JsonWriter writer(out);
        writer.begin_object();
        writer.key("digzip").value(info.program);
        writer.key("api").value(static_cast<std::int64_t>(info.api));
        writer.key("dependencies").begin_object();
        for (const auto& dep : info.dependencies) {
            writer.key(dep.name).value(dep.version);
        }
        writer.end_object();
        writer.end_object();
        out << '\n';
        return;
    }
    out << "digzip " << info.program << '\n' << "api " << info.api << '\n';
    for (const auto& dep : info.dependencies) {
        out << dep.name << ' ' << dep.version << '\n';
    }
}

void print_list(std::ostream& out, const digzip::ArchiveInfo& info)
{
    std::uint64_t files = 0;
    std::uint64_t directories = 0;
    std::uint64_t other = 0;
    std::uint64_t bytes = 0;
    out << pad_right(tr("Modified (UTC)"), 21) << pad_right(tr("Type"), 9) << pad_left(tr("Size"), 12) << "  "
        << tr("Name") << '\n';
    for (const digzip::Entry& e : info.entries) {
        out << (e.mtime ? format_time(*e.mtime) : std::string(19, ' ')) << "  "
            << pad_right(tr(digzip::to_string(e.type)), 9)
            << pad_left(e.size ? std::to_string(*e.size) : std::string(), 12) << "  " << e.path;
        if (e.type == digzip::EntryType::Symlink || e.type == digzip::EntryType::Hardlink) {
            out << " -> " << e.link_target;
        }
        if (e.encrypted) {
            out << "  " << tr("[encrypted]");
        }
        out << '\n';
        switch (e.type) {
        case digzip::EntryType::File:
            ++files;
            bytes += e.size.value_or(0);
            break;
        case digzip::EntryType::Directory:
            ++directories;
            break;
        default:
            ++other;
            break;
        }
    }
    out << fill(tr("{1}: {2}, {3}, {4}, {5}"),
                {digzip::to_string(info.format), counted(files, "file", "files"),
                 counted(directories, "directory", "directories"), counted(other, "other entry", "other entries"),
                 format_size(bytes)})
        << '\n';
}

void print_list_json(std::ostream& out, const std::filesystem::path& archive, const digzip::ArchiveInfo& info)
{
    JsonWriter json(out);
    json.begin_object();
    json.key("archive").value(path_to_utf8(archive));
    json.key("format").value(digzip::to_string(info.format));
    json.key("entries").begin_array();
    for (const digzip::Entry& e : info.entries) {
        write_entry_json(json, e);
    }
    json.end_array();
    json.end_object();
    out << '\n';
}

void print_report(std::ostream& out, std::ostream& err, const std::string& action, const digzip::Report& report,
                  bool quiet)
{
    for (const digzip::Issue& issue : report.issues) {
        if (quiet && issue.severity == digzip::Severity::Warning) {
            continue;
        }
        const std::string message = describe(issue.code, issue.message);
        err << (issue.severity == digzip::Severity::Error ? tr("error: ") : tr("warning: "))
            << (issue.path.empty() ? message : fill(tr("{1}: {2}"), {issue.path, message})) << '\n';
    }
    if (quiet) {
        return;
    }
    out << fill(tr(action == "extract" ? "Extracted {1}, {2}, {3} ({4})" : "Tested {1}, {2}, {3} ({4})"),
                {counted(report.files, "file", "files"), counted(report.directories, "directory", "directories"),
                 counted(report.links, "link", "links"), format_size(report.bytes)});
    if (report.skipped != 0) {
        out << ", " << counted(report.skipped, "entry skipped", "entries skipped");
    }
    out << (report.ok() ? "" : tr(" - with errors")) << '\n';
}

void print_report_json(std::ostream& out, const std::filesystem::path& archive, const std::string& action,
                       const digzip::Report& report)
{
    JsonWriter json(out);
    json.begin_object();
    json.key("archive").value(path_to_utf8(archive));
    json.key("action").value(action);
    json.key("format").value(digzip::to_string(report.format));
    json.key("ok").value(report.ok());
    json.key("exit_code").value(static_cast<std::int64_t>(exit_code(report)));
    json.key("files").value(report.files);
    json.key("directories").value(report.directories);
    json.key("links").value(report.links);
    json.key("skipped").value(report.skipped);
    json.key("bytes").value(report.bytes);
    json.key("issues").begin_array();
    for (const digzip::Issue& issue : report.issues) {
        json.begin_object();
        json.key("severity").value(issue.severity == digzip::Severity::Error ? "error" : "warning");
        json.key("code").value(digzip::to_string(issue.code));
        json.key("path").value(issue.path);
        json.key("message").value(issue.message);
        json.end_object();
    }
    json.end_array();
    json.end_object();
    out << '\n';
}

void print_error_json(std::ostream& out, const std::filesystem::path& archive, const digzip::Error& error)
{
    JsonWriter json(out);
    json.begin_object();
    json.key("archive").value(path_to_utf8(archive));
    json.key("ok").value(false);
    json.key("exit_code").value(static_cast<std::int64_t>(exit_code(error.code())));
    json.key("error").begin_object();
    json.key("code").value(digzip::to_string(error.code()));
    json.key("message").value(error.what());
    json.end_object();
    json.end_object();
    out << '\n';
}

void ProgressBar::update(const digzip::Progress& progress)
{
    const unsigned width = std::max(40u, terminal_width()) - 1;
    const double fraction = progress.archive_size == 0
                                ? 0.0
                                : static_cast<double>(progress.archive_read) / static_cast<double>(progress.archive_size);
    const int percent = static_cast<int>(fraction * 100.0);
    constexpr int kBar = 20;
    const int filled = static_cast<int>(fraction * kBar);

    std::ostringstream line;
    line << std::setw(3) << percent << "% [" << std::string(static_cast<std::size_t>(filled), '#')
         << std::string(static_cast<std::size_t>(kBar - filled), '.') << "] "
         << counted(progress.entries, "entry", "entries") << ", " << format_size(progress.bytes) << "  ";
    std::string text = line.str();
    if (text.size() < width) {
        text += truncate_utf8(progress.current, width - text.size());
    }
    std::cerr << '\r' << text;
    if (text.size() < drawn_) {
        std::cerr << std::string(drawn_ - text.size(), ' ');
    }
    std::cerr << std::flush;
    drawn_ = text.size();
}

void ProgressBar::clear()
{
    if (drawn_ != 0) {
        std::cerr << '\r' << std::string(drawn_, ' ') << '\r' << std::flush;
        drawn_ = 0;
    }
}

} // namespace cli
