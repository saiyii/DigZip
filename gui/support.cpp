#include "support.hpp"

#include "entry_model.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QTranslator>

#include <algorithm>

namespace digzip::gui {

namespace {

struct Text {
    Q_DECLARE_TR_FUNCTIONS(Report)
};

bool is_password_issue(const Issue& issue)
{
    return issue.code == ErrorCode::PasswordRequired || issue.code == ErrorCode::WrongPassword;
}

} // namespace

void install_translations(QCoreApplication& app)
{
    // English first, for its plural forms; a translation for the user's language takes precedence.
    auto* english = new QTranslator(&app);
    if (english->load(QStringLiteral(":/i18n/digzip_en.qm"))) {
        QCoreApplication::installTranslator(english);
    }
    auto* local = new QTranslator(&app);
    if (local->load(QLocale(), QStringLiteral("digzip"), QStringLiteral("_"), QStringLiteral(":/i18n"))) {
        QCoreApplication::installTranslator(local);
    }
}

QString default_destination(const QString& archive)
{
    static const char* const suffixes[] = {".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tgz", ".tbz2", ".tbz",
                                           ".txz",    ".tzst",    ".zip",    ".7z",      ".rar", ".tar",  ".gz",
                                           ".bz2",    ".xz",      ".zst",    ".exe"};
    const QFileInfo info(archive);
    QString name = info.fileName();
    for (const char* suffix : suffixes) {
        const QString s = QString::fromLatin1(suffix);
        if (name.size() > s.size() && name.endsWith(s, Qt::CaseInsensitive)) {
            name.chop(s.size());
            break;
        }
    }
    if (name == info.fileName()) {
        name += Text::tr(" (extracted)");
    }
    return info.absoluteDir().filePath(name);
}

PasswordRetry password_retry(const Report& report)
{
    PasswordRetry retry;
    for (const Issue& issue : report.issues) {
        if (!is_password_issue(issue)) {
            continue;
        }
        retry.needed = true;
        if (issue.path.empty()) {
            retry.whole_archive = true;
        } else {
            retry.paths.push_back(issue.path);
        }
    }
    if (retry.whole_archive) {
        retry.paths.clear();
    }
    return retry;
}

void merge_retry(Report& report, const Report& retry)
{
    report.issues.erase(std::remove_if(report.issues.begin(), report.issues.end(), is_password_issue),
                        report.issues.end());
    report.files += retry.files;
    report.directories += retry.directories;
    report.links += retry.links;
    report.skipped += retry.skipped;
    report.bytes += retry.bytes;
    report.issues.insert(report.issues.end(), retry.issues.begin(), retry.issues.end());
}

QString summary(const Report& report, bool extracting)
{
    const auto count = [&](Severity severity) {
        return static_cast<int>(std::count_if(report.issues.begin(), report.issues.end(),
                                              [&](const Issue& i) { return i.severity == severity; }));
    };
    const int files = static_cast<int>(report.files);
    QString text = extracting ? Text::tr("Extracted %n file(s)", nullptr, files) : Text::tr("Verified %n file(s)", nullptr, files);
    text += QStringLiteral(" (%1)").arg(format_size(report.bytes));

    const bool cancelled = std::any_of(report.issues.begin(), report.issues.end(),
                                       [](const Issue& i) { return i.code == ErrorCode::Cancelled; });
    if (cancelled) {
        return text + Text::tr(", cancelled");
    }
    if (const int errors = count(Severity::Error)) {
        text += Text::tr(", %n error(s)", nullptr, errors);
    }
    if (const int warnings = count(Severity::Warning)) {
        text += Text::tr(", %n warning(s)", nullptr, warnings);
    }
    return text;
}

QStringList issue_lines(const Report& report)
{
    QStringList lines;
    for (const Issue& issue : report.issues) {
        if (issue.code == ErrorCode::Cancelled) {
            continue;
        }
        const QString level = issue.severity == Severity::Error ? Text::tr("Error") : Text::tr("Warning");
        const QString message = QString::fromUtf8(issue.message.c_str());
        lines << (issue.path.empty() ? QStringLiteral("%1: %2").arg(level, message)
                                     : QStringLiteral("%1: %2: %3")
                                           .arg(level, QString::fromUtf8(issue.path.c_str()), message));
    }
    return lines;
}

} // namespace digzip::gui
