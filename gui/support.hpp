#pragma once

#include <digzip/archive.hpp>

#include <QCoreApplication>
#include <QString>
#include <QStringList>

#include <string>
#include <vector>

namespace digzip::gui {

// Installs the embedded translations into `app`.
void install_translations(QCoreApplication& app);

// Folder proposed for extraction: next to the archive, named after it without its archive
// extensions ("photos.tar.gz" -> "photos").
QString default_destination(const QString& archive);

// Entries that failed for want of the right password, to be tried again with another one.
struct PasswordRetry {
    bool needed = false;
    bool whole_archive = false;     // the archive itself, e.g. its file names, is encrypted
    std::vector<std::string> paths; // otherwise, the entries to try again
};
PasswordRetry password_retry(const Report& report);

// Removes the password issues, kept for the retry, and adds the result of the retry.
void merge_retry(Report& report, const Report& retry);

// One line for the status bar, e.g. "Extracted 12 files (3.4 MB), 1 warning".
QString summary(const Report& report, bool extracting);

// One line per issue, for the problem panel.
QStringList issue_lines(const Report& report);

} // namespace digzip::gui
