#pragma once

#include <digzip/archive.hpp>

#include <filesystem>
#include <ostream>
#include <string>

namespace cli {

enum ExitCode : int {
    kOk = 0,
    kWarnings = 1,
    kUsage = 2,
    kIoError = 3,
    kDataError = 4,
    kRefused = 5,
    kPassword = 6,
    kInterrupted = 130,
};

int exit_code(digzip::ErrorCode code);
int exit_code(const digzip::Report& report);

void print_version(std::ostream& out, bool json);

void print_list(std::ostream& out, const digzip::ArchiveInfo& info);
void print_list_json(std::ostream& out, const std::filesystem::path& archive, const digzip::ArchiveInfo& info);

// `action` is "extract" or "test".
void print_report(std::ostream& out, std::ostream& err, const std::string& action, const digzip::Report& report,
                  bool quiet);
void print_report_json(std::ostream& out, const std::filesystem::path& archive, const std::string& action,
                       const digzip::Report& report);

void print_error_json(std::ostream& out, const std::filesystem::path& archive, const digzip::Error& error);

// Single-line progress display on standard error, redrawn in place.
class ProgressBar {
public:
    void update(const digzip::Progress& progress);
    void clear();

private:
    std::size_t drawn_ = 0;
};

} // namespace cli
