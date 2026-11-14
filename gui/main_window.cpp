#include "main_window.hpp"

#include "support.hpp"

#include <digzip/version.hpp>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QTableView>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

namespace digzip::gui {

namespace {

QString local_file(const QMimeData* mime)
{
    if (mime == nullptr) {
        return {};
    }
    for (const QUrl& url : mime->urls()) {
        if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile()) {
            return url.toLocalFile();
        }
    }
    return {};
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    model_ = new EntryModel(this);
    job_ = new Job(this);
    ask_password = [this](bool wrong) { return prompt_password(wrong); };

    connect(job_, &Job::listed, this, &MainWindow::on_listed);
    connect(job_, &Job::listFailed, this, &MainWindow::on_list_failed);
    connect(job_, &Job::finished, this, &MainWindow::on_finished);
    connect(job_, &Job::progress, this, &MainWindow::on_progress);

    build_ui();
    setAcceptDrops(true);
    set_idle(tr("Open an archive, or drop one on this window."));
}

void MainWindow::build_ui()
{
    setWindowTitle(QStringLiteral("DigZip"));
    resize(900, 600);

    open_action_ = new QAction(style()->standardIcon(QStyle::SP_DialogOpenButton), tr("&Open..."), this);
    open_action_->setShortcut(QKeySequence::Open);
    connect(open_action_, &QAction::triggered, this, &MainWindow::choose_archive);

    extract_action_ = new QAction(QIcon(QStringLiteral(":/icons/extract.svg")), tr("&Extract"), this);
    extract_action_->setShortcut(Qt::CTRL | Qt::Key_E);
    connect(extract_action_, &QAction::triggered, this, &MainWindow::start_extract);

    test_action_ = new QAction(style()->standardIcon(QStyle::SP_DialogApplyButton), tr("&Test"), this);
    test_action_->setShortcut(Qt::CTRL | Qt::Key_T);
    connect(test_action_, &QAction::triggered, this, &MainWindow::start_test);

    cancel_action_ = new QAction(style()->standardIcon(QStyle::SP_BrowserStop), tr("&Cancel"), this);
    cancel_action_->setShortcut(Qt::Key_Escape);
    connect(cancel_action_, &QAction::triggered, this, &MainWindow::cancel);

    auto* quit = new QAction(tr("&Quit"), this);
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    QMenu* file_menu = menuBar()->addMenu(tr("&File"));
    file_menu->addAction(open_action_);
    file_menu->addSeparator();
    file_menu->addAction(quit);
    QMenu* archive_menu = menuBar()->addMenu(tr("&Archive"));
    archive_menu->addAction(extract_action_);
    archive_menu->addAction(test_action_);
    archive_menu->addAction(cancel_action_);
    QMenu* options_menu = menuBar()->addMenu(tr("&Options"));
    QMenu* language_menu = options_menu->addMenu(tr("&Language"));
    auto* languages = new QActionGroup(this);
    const QString saved = saved_language();
    const std::pair<QString, QString> choices[] = {
        {QString(), tr("System default")},
        {QStringLiteral("en"), QStringLiteral("English")},
        {QStringLiteral("fr"), QStringLiteral("Français")},
    };
    for (const auto& [code, name] : choices) {
        QAction* action = language_menu->addAction(name);
        action->setCheckable(true);
        action->setChecked(code == saved);
        languages->addAction(action);
        connect(action, &QAction::triggered, this, [this, language = code] {
            save_language(language);
            QMessageBox::information(this, tr("Language"),
                                     tr("DigZip will use this language the next time it starts."));
        });
    }

    QMenu* help_menu = menuBar()->addMenu(tr("&Help"));
    help_menu->addAction(tr("&About DigZip"), this, &MainWindow::about);
    help_menu->addAction(tr("About &Qt"), qApp, &QApplication::aboutQt);

    QToolBar* toolbar = addToolBar(tr("Main"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar->addAction(open_action_);
    toolbar->addAction(extract_action_);
    toolbar->addAction(test_action_);
    toolbar->addAction(cancel_action_);

    // Page 0: nothing open yet. Page 1: the archive content.
    auto* hint = new QLabel(tr("Drop an archive here\nor use Open to choose one."));
    hint->setAlignment(Qt::AlignCenter);
    QFont hint_font = hint->font();
    hint_font.setPointSizeF(hint_font.pointSizeF() * 1.4);
    hint->setFont(hint_font);
    hint->setEnabled(false);

    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setSortRole(EntryModel::SortRole);
    proxy_->setSortCaseSensitivity(Qt::CaseInsensitive);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy_->setFilterKeyColumn(EntryModel::Name);

    filter_ = new QLineEdit;
    filter_->setObjectName(QStringLiteral("filter"));
    filter_->setPlaceholderText(tr("Filter by name"));
    filter_->setClearButtonEnabled(true);
    connect(filter_, &QLineEdit::textChanged, proxy_, &QSortFilterProxyModel::setFilterFixedString);

    table_ = new QTableView;
    table_->setObjectName(QStringLiteral("entries"));
    table_->setModel(proxy_);
    table_->setSortingEnabled(true);
    table_->sortByColumn(EntryModel::Name, Qt::AscendingOrder);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->setWordWrap(false);
    table_->verticalHeader()->hide();
    table_->horizontalHeader()->setSectionResizeMode(EntryModel::Name, QHeaderView::Stretch);
    table_->horizontalHeader()->setHighlightSections(false);

    auto* content = new QWidget;
    auto* content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->addWidget(filter_);
    content_layout->addWidget(table_);

    pages_ = new QStackedWidget;
    pages_->addWidget(hint);
    pages_->addWidget(content);

    destination_ = new QLineEdit;
    destination_->setObjectName(QStringLiteral("destination"));
    auto* browse = new QPushButton(tr("Browse..."));
    connect(browse, &QPushButton::clicked, this, &MainWindow::choose_destination);

    overwrite_ = new QComboBox;
    overwrite_->setObjectName(QStringLiteral("overwrite"));
    overwrite_->addItem(tr("Keep existing files"), static_cast<int>(Overwrite::Skip));
    overwrite_->addItem(tr("Replace existing files"), static_cast<int>(Overwrite::Replace));
    overwrite_->addItem(tr("Keep both (rename)"), static_cast<int>(Overwrite::Rename));

    auto* destination_row = new QHBoxLayout;
    destination_row->addWidget(new QLabel(tr("Extract to:")));
    destination_row->addWidget(destination_, 1);
    destination_row->addWidget(browse);
    destination_row->addWidget(overwrite_);

    issues_ = new QPlainTextEdit;
    issues_->setObjectName(QStringLiteral("issues"));
    issues_->setReadOnly(true);
    issues_->setMaximumHeight(120);
    issues_->hide();

    auto* central = new QWidget;
    auto* layout = new QVBoxLayout(central);
    layout->addWidget(pages_, 1);
    layout->addWidget(issues_);
    layout->addLayout(destination_row);
    setCentralWidget(central);

    status_ = new QLabel;
    status_->setObjectName(QStringLiteral("status"));
    progress_ = new QProgressBar;
    progress_->setMaximumWidth(240);
    progress_->setTextVisible(false);
    progress_->hide();
    open_folder_ = new QPushButton(tr("Open folder"));
    open_folder_->hide();
    connect(open_folder_, &QPushButton::clicked, this,
            [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(destination())); });
    statusBar()->setStyleSheet(QStringLiteral("QStatusBar::item { border: none; }"));
    statusBar()->addWidget(status_, 1);
    statusBar()->addPermanentWidget(open_folder_);
    statusBar()->addPermanentWidget(progress_);
}

QString MainWindow::destination() const
{
    return destination_->text().trimmed();
}

void MainWindow::set_destination(const QString& path)
{
    destination_->setText(QDir::toNativeSeparators(path));
}

void MainWindow::set_overwrite(Overwrite overwrite)
{
    overwrite_->setCurrentIndex(overwrite_->findData(static_cast<int>(overwrite)));
}

void MainWindow::open_archive(const QString& path)
{
    if (busy()) {
        return;
    }
    archive_ = QFileInfo(path).absoluteFilePath();
    passwords_.clear();
    model_->set_entries({});
    filter_->clear();
    setWindowTitle(QStringLiteral("%1 - DigZip").arg(QFileInfo(archive_).fileName()));
    set_destination(default_destination(archive_));
    run_list();
}

void MainWindow::run_list()
{
    set_busy(Mode::List, tr("Reading %1...").arg(QFileInfo(archive_).fileName()));
    Options options;
    options.passwords = passwords_;
    job_->list(archive_, options);
}

void MainWindow::start_extract()
{
    if (busy() || model_->rowCount() == 0) {
        return;
    }
    if (destination().isEmpty()) {
        choose_destination();
        if (destination().isEmpty()) {
            return;
        }
    }
    if (model_->any_encrypted() && passwords_.empty()) {
        const auto password = ask_password(false);
        if (!password) {
            return;
        }
        passwords_ = {password->toStdString()};
    }
    mode_ = Mode::Extract;
    retrying_ = false;
    run({});
}

void MainWindow::start_test()
{
    if (busy() || model_->rowCount() == 0) {
        return;
    }
    if (model_->any_encrypted() && passwords_.empty()) {
        const auto password = ask_password(false);
        if (!password) {
            return;
        }
        passwords_ = {password->toStdString()};
    }
    mode_ = Mode::Test;
    retrying_ = false;
    run({});
}

void MainWindow::run(const std::vector<std::string>& include)
{
    const QString name = QFileInfo(archive_).fileName();
    if (mode_ == Mode::Extract) {
        set_busy(Mode::Extract, tr("Extracting %1...").arg(name));
        ExtractOptions options;
        options.passwords = passwords_;
        options.include = include;
        options.overwrite = static_cast<Overwrite>(overwrite_->currentData().toInt());
        job_->extract(archive_, destination(), options);
    } else {
        set_busy(Mode::Test, tr("Testing %1...").arg(name));
        Options options;
        options.passwords = passwords_;
        options.include = include;
        job_->test(archive_, options);
    }
}

void MainWindow::cancel()
{
    if (busy()) {
        job_->cancel();
        status_->setText(tr("Cancelling..."));
    }
}

void MainWindow::on_listed(const ArchiveInfo& info)
{
    model_->set_entries(info.entries);
    pages_->setCurrentIndex(1);
    table_->resizeColumnsToContents();
    table_->horizontalHeader()->setSectionResizeMode(EntryModel::Name, QHeaderView::Stretch);
    set_idle(tr("%n entry(s), %1", nullptr, static_cast<int>(info.entries.size()))
                 .arg(QString::fromStdString(to_string(info.format))));
}

void MainWindow::on_list_failed(ErrorCode code, const QString& message)
{
    if (code == ErrorCode::PasswordRequired || code == ErrorCode::WrongPassword) {
        if (const auto password = ask_password(code == ErrorCode::WrongPassword)) {
            passwords_ = {password->toStdString()};
            run_list();
            return;
        }
    }
    archive_.clear();
    destination_->clear();
    pages_->setCurrentIndex(0);
    setWindowTitle(QStringLiteral("DigZip"));
    show_issues({engine_message(code, message)});
    set_idle(code == ErrorCode::Cancelled ? tr("Cancelled.") : tr("This archive cannot be opened."));
}

void MainWindow::on_finished(const Report& report)
{
    if (retrying_) {
        merge_retry(report_, report);
    } else {
        report_ = report;
    }

    const PasswordRetry retry = password_retry(report);
    if (retry.needed) {
        if (const auto password = ask_password(!passwords_.empty())) {
            passwords_ = {password->toStdString()};
            retrying_ = true;
            run(retry.paths);
            return;
        }
    }

    const bool extracting = mode_ == Mode::Extract;
    show_issues(issue_lines(report_));
    set_idle(summary(report_, extracting));
    open_folder_->setVisible(extracting && (report_.files + report_.directories) > 0);
}

void MainWindow::on_progress(double fraction, const QString& current)
{
    if (fraction < 0) {
        progress_->setRange(0, 0);
    } else {
        progress_->setRange(0, 1000);
        progress_->setValue(static_cast<int>(fraction * 1000));
    }
    if (!current.isEmpty()) {
        status_->setText(current);
    }
}

void MainWindow::set_busy(Mode mode, const QString& status)
{
    mode_ = mode;
    status_->setText(status);
    progress_->setRange(0, 0);
    progress_->show();
    open_folder_->hide();
    if (!retrying_) {
        show_issues({});
    }
    update_actions();
}

void MainWindow::set_idle(const QString& status)
{
    status_->setText(status);
    progress_->hide();
    retrying_ = false;
    const bool was_running = mode_ != Mode::None;
    mode_ = Mode::None;
    update_actions();
    if (was_running) {
        emit operationFinished();
    }
}

void MainWindow::show_issues(const QStringList& lines)
{
    issues_->setPlainText(lines.join(QLatin1Char('\n')));
    issues_->setVisible(!lines.isEmpty());
}

void MainWindow::update_actions()
{
    const bool running = mode_ != Mode::None;
    const bool ready = !running && !archive_.isEmpty() && model_->rowCount() > 0;
    open_action_->setEnabled(!running);
    extract_action_->setEnabled(ready);
    test_action_->setEnabled(ready);
    cancel_action_->setEnabled(running);
    destination_->setEnabled(!running);
    overwrite_->setEnabled(!running);
}

std::optional<QString> MainWindow::prompt_password(bool wrong)
{
    const QString label = wrong ? tr("Wrong password. Enter the password of %1:")
                                : tr("%1 is encrypted. Enter its password:");
    bool ok = false;
    const QString password = QInputDialog::getText(this, tr("Password"), label.arg(QFileInfo(archive_).fileName()),
                                                   QLineEdit::Password, {}, &ok);
    if (!ok || password.isEmpty()) {
        return std::nullopt;
    }
    return password;
}

void MainWindow::choose_archive()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open an archive"), {},
        tr("Archives (*.zip *.7z *.rar *.tar *.tgz *.tbz2 *.txz *.tzst *.gz *.bz2 *.xz *.zst *.exe);;All files (*)"));
    if (!path.isEmpty()) {
        open_archive(path);
    }
}

void MainWindow::choose_destination()
{
    const QString path = QFileDialog::getExistingDirectory(this, tr("Extract to"), destination());
    if (!path.isEmpty()) {
        set_destination(path);
    }
}

void MainWindow::about()
{
    const VersionInfo info = version_info();
    QString libraries;
    for (const Dependency& d : info.dependencies) {
        libraries += QStringLiteral("<br>%1 %2").arg(QString::fromStdString(d.name), QString::fromStdString(d.version));
    }
    QMessageBox::about(this, tr("About DigZip"),
                       tr("<h3>DigZip %1</h3>"
                          "<p>Fast, multi-format, open source archive extractor.</p>"
                          "<p>MIT or Apache-2.0 license.</p>"
                          "<p>Libraries:%2<br>Qt %3 (LGPLv3)</p>")
                           .arg(QString::fromStdString(info.program), libraries, QString::fromLatin1(qVersion())));
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (!busy() && !local_file(event->mimeData()).isEmpty()) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent* event)
{
    const QString path = local_file(event->mimeData());
    if (!path.isEmpty()) {
        event->acceptProposedAction();
        open_archive(path);
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    job_->cancel();
    event->accept();
}

} // namespace digzip::gui
