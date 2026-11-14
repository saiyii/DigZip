#include "entry_model.hpp"
#include "job.hpp"
#include "main_window.hpp"
#include "support.hpp"

#include <QApplication>
#include <algorithm>
#include <QDir>
#include <QDropEvent>
#include <QFile>
#include <QMenuBar>
#include <QTranslator>
#include <QIcon>
#include <QLocale>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

using namespace digzip;
using namespace digzip::gui;

namespace {

QString fixture(const char* name)
{
    return QDir(QStringLiteral(DIGZIP_FIXTURES_DIR)).filePath(QString::fromLatin1(name));
}

QByteArray read(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// Waits for the window to finish its current operation.
bool wait_idle(MainWindow& window, QSignalSpy& done)
{
    return done.wait(30'000) && !window.busy();
}

} // namespace

class GuiTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // The tests check English texts, whatever the language of the machine.
        qputenv("DIGZIP_LANG", "en");
        QCOMPARE(install_translations(*QCoreApplication::instance()), QStringLiteral("en"));
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    }

    void the_language_follows_the_override_then_the_choice_then_the_system()
    {
        const QStringList french_system{QStringLiteral("fr-FR"), QStringLiteral("en-US")};
        QCOMPARE(ui_language({}, {}, french_system), QStringLiteral("fr"));
        QCOMPARE(ui_language({}, QStringLiteral("en"), french_system), QStringLiteral("en"));
        QCOMPARE(ui_language(QStringLiteral("fr"), QStringLiteral("en"), {}), QStringLiteral("fr"));
        QCOMPARE(ui_language(QStringLiteral("de"), {}, {QStringLiteral("de-DE"), QStringLiteral("fr-CH")}),
                 QStringLiteral("fr"));
        QCOMPARE(ui_language({}, {}, {QStringLiteral("ja-JP")}), QStringLiteral("en"));
        QCOMPARE(ui_language({}, {}, {}), QStringLiteral("en"));
    }

    void french_translation_is_complete_and_used()
    {
        QTranslator french;
        QVERIFY(french.load(QStringLiteral(":/i18n/digzip_fr.qm")));
        QCoreApplication::installTranslator(&french);

        Report report;
        report.files = 1;
        QVERIFY(summary(report, true).startsWith(QStringLiteral("1 fichier extrait")));
        report.files = 3;
        report.issues = {{Severity::Warning, ErrorCode::AlreadyExists, "a.txt", "already exists, skipped"}};
        QVERIFY(summary(report, false).startsWith(QStringLiteral("3 fichiers vérifiés")));
        QCOMPARE(issue_lines(report),
                 QStringList{QStringLiteral("Avertissement : a.txt : existe déjà (already exists, skipped)")});
        QCOMPARE(EntryModel().headerData(EntryModel::Size, Qt::Horizontal).toString(), QStringLiteral("Taille"));

        MainWindow window;
        QVERIFY(window.menuBar()->actions().first()->text() == QStringLiteral("&Fichier"));

        QCoreApplication::removeTranslator(&french);
        QCOMPARE(issue_lines(report), QStringList{QStringLiteral("Warning: a.txt: already exists, skipped")});
    }


    void default_destination_strips_archive_extensions()
    {
        const QString dir = QDir::current().absolutePath();
        QCOMPARE(default_destination(dir + QStringLiteral("/photos.tar.gz")), dir + QStringLiteral("/photos"));
        QCOMPARE(default_destination(dir + QStringLiteral("/a.b.ZIP")), dir + QStringLiteral("/a.b"));
        QCOMPARE(default_destination(dir + QStringLiteral("/notes.txt.zst")), dir + QStringLiteral("/notes.txt"));
        QCOMPARE(default_destination(dir + QStringLiteral("/archive")), dir + QStringLiteral("/archive (extracted)"));
        QCOMPARE(default_destination(dir + QStringLiteral("/.zip")), dir + QStringLiteral("/.zip (extracted)"));
    }

    void password_retry_collects_the_failed_entries()
    {
        Report report;
        report.files = 2;
        report.issues = {{Severity::Error, ErrorCode::WrongPassword, "a.txt", "wrong password"},
                         {Severity::Warning, ErrorCode::AlreadyExists, "b.txt", "exists"},
                         {Severity::Error, ErrorCode::PasswordRequired, "c.txt", "password required"}};
        const PasswordRetry retry = password_retry(report);
        QVERIFY(retry.needed);
        QVERIFY(!retry.whole_archive);
        QCOMPARE(retry.paths, (std::vector<std::string>{"a.txt", "c.txt"}));

        Report second;
        second.files = 2;
        merge_retry(report, second);
        QCOMPARE(report.files, 4u);
        QCOMPARE(report.issues.size(), std::size_t(1));
        QCOMPARE(report.issues[0].code, ErrorCode::AlreadyExists);

        report.issues.push_back({Severity::Error, ErrorCode::PasswordRequired, {}, "encrypted headers"});
        QVERIFY(password_retry(report).whole_archive);
        QVERIFY(!password_retry(Report{}).needed);
    }

    void summary_counts_files_and_issues()
    {
        Report report;
        report.files = 3;
        report.issues = {{Severity::Warning, ErrorCode::AlreadyExists, "x", "exists"}};
        const QString text = summary(report, true);
        QVERIFY2(text.startsWith(QStringLiteral("Extracted 3 files")), qPrintable(text));
        QVERIFY2(text.endsWith(QStringLiteral("1 warning")), qPrintable(text));
        QVERIFY(summary(Report{}, false).startsWith(QStringLiteral("Verified 0 files")));
        report.files = 1;
        QVERIFY(summary(report, true).startsWith(QStringLiteral("Extracted 1 file (")));
        QCOMPARE(issue_lines(report), QStringList{QStringLiteral("Warning: x: exists")});

        report.issues.push_back({Severity::Error, ErrorCode::Cancelled, {}, "cancelled"});
        QVERIFY(summary(report, true).endsWith(QStringLiteral("cancelled")));
        QCOMPARE(issue_lines(report).size(), 1);
    }

    void model_shows_the_entries()
    {
        EntryModel model;
        model.set_entries(list(to_path(fixture("basic-deflate.zip"))).entries);
        QVERIFY(model.rowCount() >= 6);
        QCOMPARE(model.columnCount(), int(EntryModel::ColumnCount));
        QVERIFY(!model.any_encrypted());

        int hello = -1;
        for (int row = 0; row < model.rowCount(); ++row) {
            if (model.index(row, EntryModel::Name).data().toString() == QStringLiteral("hello.txt")) {
                hello = row;
            }
        }
        QVERIFY(hello >= 0);
        QCOMPARE(model.index(hello, EntryModel::Size).data(EntryModel::SortRole).toLongLong(), 15);
        QVERIFY(!model.index(hello, EntryModel::Size).data().toString().isEmpty());
        QVERIFY(!model.index(hello, EntryModel::Modified).data().toString().isEmpty());
        QCOMPARE(model.headerData(EntryModel::Name, Qt::Horizontal).toString(), QStringLiteral("Name"));
    }

    void job_lists_extracts_and_reports_failures()
    {
        Job job;
        QSignalSpy listed(&job, &Job::listed);
        QSignalSpy failed(&job, &Job::listFailed);
        QSignalSpy finished(&job, &Job::finished);

        job.list(fixture("basic.7z"), {});
        QVERIFY(listed.wait(30'000));
        QVERIFY(!job.running());

        job.list(fixture("encrypted-headers.rar"), {});
        QVERIFY(failed.wait(30'000));
        QCOMPARE(failed.at(0).at(0).value<ErrorCode>(), ErrorCode::PasswordRequired);

        job.list(fixture("garbage.bin"), {});
        QVERIFY(failed.wait(30'000));

        QTemporaryDir out;
        job.extract(fixture("basic.rar"), out.path(), {});
        QVERIFY(finished.wait(30'000));
        QVERIFY(finished.at(0).at(0).value<Report>().ok());
        QCOMPARE(read(out.filePath(QStringLiteral("hello.txt"))), QByteArray("Hello, DigZip!\n"));
    }

    void job_reports_progress_and_can_be_cancelled()
    {
        Job job;
        QSignalSpy progress(&job, &Job::progress);
        QSignalSpy finished(&job, &Job::finished);
        job.test(fixture("large.rar"), {});
        QVERIFY(finished.wait(30'000));
        QVERIFY(finished.at(0).at(0).value<Report>().ok());
        QVERIFY(!progress.isEmpty());

        QTemporaryDir out;
        job.extract(fixture("large.rar"), out.path(), {});
        job.cancel();
        QVERIFY(finished.wait(30'000));
        const Report report = finished.at(1).at(0).value<Report>();
        const bool cancelled = std::any_of(report.issues.begin(), report.issues.end(),
                                           [](const Issue& i) { return i.code == ErrorCode::Cancelled; });
        QVERIFY(cancelled || report.ok()); // the extraction may end before the request is seen
        const QStringList left = QDir(out.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
        for (const QString& name : left) {
            QVERIFY2(!name.contains(QStringLiteral(".digzip-")), qPrintable(name));
        }
    }

    void destroying_a_running_job_stops_it()
    {
        QTemporaryDir out;
        auto* job = new Job;
        job->extract(fixture("large.rar"), out.path(), {});
        delete job; // must cancel and wait, not crash
    }

    void window_icons_are_embedded()
    {
        QVERIFY(!QIcon(QStringLiteral(":/icons/extract.svg")).pixmap(24, 24).isNull());
    }

    void window_opens_and_extracts()
    {
        MainWindow window;
        QSignalSpy done(&window, &MainWindow::operationFinished);
        window.open_archive(fixture("basic.7z"));
        QVERIFY(wait_idle(window, done));
        QVERIFY(window.model().rowCount() >= 6);
        QVERIFY(window.destination().endsWith(QStringLiteral("basic")));

        QTemporaryDir out;
        window.set_destination(out.path());
        window.start_extract();
        QVERIFY(wait_idle(window, done));
        QCOMPARE(read(out.filePath(QStringLiteral("hello.txt"))), QByteArray("Hello, DigZip!\n"));
        QVERIFY(window.findChild<QPlainTextEdit*>(QStringLiteral("issues"))->isHidden());

        // Again: the files exist and are kept, with one warning each.
        window.start_extract();
        QVERIFY(wait_idle(window, done));
        const auto* issues = window.findChild<QPlainTextEdit*>(QStringLiteral("issues"));
        QVERIFY(!issues->isHidden());
        QVERIFY(issues->toPlainText().contains(QStringLiteral("hello.txt")));

        window.set_overwrite(Overwrite::Rename);
        window.start_extract();
        QVERIFY(wait_idle(window, done));
        QVERIFY(QFile::exists(out.filePath(QStringLiteral("hello (1).txt"))));
    }

    void window_asks_again_after_a_wrong_password()
    {
        MainWindow window;
        std::vector<bool> asked;
        window.ask_password = [&](bool wrong) -> std::optional<QString> {
            asked.push_back(wrong);
            return asked.size() == 1 ? QStringLiteral("nope") : QStringLiteral("secret");
        };
        QSignalSpy done(&window, &MainWindow::operationFinished);
        window.open_archive(fixture("encrypted.rar"));
        QVERIFY(wait_idle(window, done));
        QVERIFY(window.model().any_encrypted());

        QTemporaryDir out;
        window.set_destination(out.path());
        window.start_extract();
        QVERIFY(wait_idle(window, done));
        QCOMPARE(asked, (std::vector<bool>{false, true}));
        QCOMPARE(read(out.filePath(QStringLiteral("secret.txt"))), QByteArray("top secret\n"));
        QVERIFY(window.findChild<QPlainTextEdit*>(QStringLiteral("issues"))->isHidden());
    }

    void window_opens_archives_with_encrypted_names()
    {
        MainWindow window;
        int asked = 0;
        window.ask_password = [&](bool) -> std::optional<QString> {
            ++asked;
            return QStringLiteral("secret");
        };
        QSignalSpy done(&window, &MainWindow::operationFinished);
        window.open_archive(fixture("encrypted-headers.rar"));
        QVERIFY(wait_idle(window, done));
        QCOMPARE(asked, 1);
        QVERIFY(window.model().rowCount() > 0);

        QTemporaryDir out;
        window.set_destination(out.path());
        window.start_extract();
        QVERIFY(wait_idle(window, done));
        QCOMPARE(asked, 1); // the password is remembered
        QCOMPARE(read(out.filePath(QStringLiteral("secret.txt"))), QByteArray("top secret\n"));
    }

    void window_reports_unreadable_files_and_declined_passwords()
    {
        MainWindow window;
        window.ask_password = [](bool) { return std::optional<QString>(); };
        QSignalSpy done(&window, &MainWindow::operationFinished);

        window.open_archive(fixture("garbage.bin"));
        QVERIFY(wait_idle(window, done));
        QCOMPARE(window.model().rowCount(), 0);
        QVERIFY(!window.findChild<QPlainTextEdit*>(QStringLiteral("issues"))->isHidden());

        window.open_archive(fixture("encrypted-headers.rar"));
        QVERIFY(wait_idle(window, done));
        QCOMPARE(window.model().rowCount(), 0);
    }

    void window_opens_a_dropped_file()
    {
        MainWindow window;
        QSignalSpy done(&window, &MainWindow::operationFinished);
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(fixture("basic.tar.gz"))});
        QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &drop);
        QVERIFY(wait_idle(window, done));
        QVERIFY(window.model().rowCount() >= 6);
    }
};

QTEST_MAIN(GuiTest)
#include "gui_test.moc"
