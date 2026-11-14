#include "support.hpp"

#include "entry_model.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QLocale>
#include <QSettings>
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

QString ui_language(const QString& forced, const QString& saved, const QStringList& system_languages)
{
    const auto supported = [](const QString& code) -> QString {
        const QString language = code.left(2).toLower();
        return language == QLatin1String("fr") || language == QLatin1String("en") ? language : QString();
    };
    for (const QString& choice : {forced, saved}) {
        if (const QString language = supported(choice); !language.isEmpty()) {
            return language;
        }
    }
    for (const QString& code : system_languages) {
        if (const QString language = supported(code); !language.isEmpty()) {
            return language;
        }
    }
    return QStringLiteral("en");
}

QString saved_language()
{
    return QSettings().value(QStringLiteral("language")).toString();
}

void save_language(const QString& language)
{
    QSettings().setValue(QStringLiteral("language"), language);
}

QString install_translations(QCoreApplication& app)
{
    const QString language = ui_language(qEnvironmentVariable("DIGZIP_LANG"), saved_language(),
                                         QLocale::system().uiLanguages());
    // Sizes and dates follow the system, unless the interface uses another language.
    if (QLocale::system().name().left(2) != language) {
        QLocale::setDefault(QLocale(language));
    }

    // Installs the first of `files` found in one of `directories`.
    const auto install = [&app](const QStringList& files, const QStringList& directories) {
        auto* translator = new QTranslator(&app);
        for (const QString& directory : directories) {
            for (const QString& file : files) {
                if (translator->load(file, directory)) {
                    QCoreApplication::installTranslator(translator);
                    return;
                }
            }
        }
        delete translator;
    };
    // Qt's own texts (standard buttons and dialogs): qt_xx.qm as windeployqt writes it, or qtbase_xx.qm.
    const QString here = QCoreApplication::applicationDirPath();
    install({QStringLiteral("qt_") + language, QStringLiteral("qtbase_") + language},
            {here + QStringLiteral("/translations"), here + QStringLiteral("/../Resources/translations"),
             QLibraryInfo::path(QLibraryInfo::TranslationsPath)});
    // English plural forms first; the French translation, installed last, takes precedence.
    install({QStringLiteral("digzip_en")}, {QStringLiteral(":/i18n")});
    if (language != QLatin1String("en")) {
        install({QStringLiteral("digzip_") + language}, {QStringLiteral(":/i18n")});
    }
    return language;
}

QString describe(ErrorCode code)
{
    switch (code) {
    case ErrorCode::Io: return Text::tr("input/output error");
    case ErrorCode::Corrupt: return Text::tr("damaged data");
    case ErrorCode::Unsupported: return Text::tr("not supported");
    case ErrorCode::Checksum: return Text::tr("checksum mismatch");
    case ErrorCode::Security: return Text::tr("refused for safety");
    case ErrorCode::LimitExceeded: return Text::tr("safety limit reached");
    case ErrorCode::InvalidName: return Text::tr("invalid name");
    case ErrorCode::AlreadyExists: return Text::tr("already exists");
    case ErrorCode::InvalidArgument: return Text::tr("invalid option");
    case ErrorCode::Cancelled: return Text::tr("cancelled");
    case ErrorCode::PasswordRequired: return Text::tr("password required");
    case ErrorCode::WrongPassword: return Text::tr("wrong password");
    }
    return Text::tr("input/output error");
}

QString engine_message(ErrorCode code, const QString& message)
{
    // The engine writes in English: a translation may put what the problem means first.
    const QString pattern = Text::tr("%1", "an engine message in English; %2 is what it means, translated");
    return pattern.contains(QLatin1String("%2")) ? pattern.arg(message, describe(code)) : pattern.arg(message);
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
        const QString message = engine_message(issue.code, QString::fromUtf8(issue.message.c_str()));
        lines << (issue.path.empty() ? Text::tr("%1: %2").arg(level, message)
                                     : Text::tr("%1: %2: %3")
                                           .arg(level, QString::fromUtf8(issue.path.c_str()), message));
    }
    return lines;
}

} // namespace digzip::gui
