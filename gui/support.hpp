#pragma once

#include <digzip/archive.hpp>

#include <QCoreApplication>
#include <QString>
#include <QStringList>

#include <string>
#include <vector>

namespace digzip::gui {

// Language of the interface, "en" or "fr": DIGZIP_LANG when set (`forced`), otherwise the
// language chosen in the Language menu (`saved`), otherwise the first supported language of the
// system's list.
QString ui_language(const QString& forced, const QString& saved, const QStringList& system_languages);

// Chooses the language and installs the matching translations, DigZip's and Qt's own, into `app`.
// Returns the language.
QString install_translations(QCoreApplication& app);

// Where the Language menu keeps its choice: "" (the system's), "en" or "fr".
QString saved_language();
void save_language(const QString& language);

// What a problem of this kind means, in the interface language.
QString describe(ErrorCode code);

// An engine message (in English) as shown in the interface language.
QString engine_message(ErrorCode code, const QString& message);

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
