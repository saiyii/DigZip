#pragma once

#include "entry_model.hpp"
#include "job.hpp"

#include <QMainWindow>

#include <functional>
#include <optional>

class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSortFilterProxyModel;
class QStackedWidget;
class QTableView;

namespace digzip::gui {

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    void open_archive(const QString& path);
    void start_extract();
    void start_test();
    void cancel();

    QString destination() const;
    void set_destination(const QString& path);
    void set_overwrite(Overwrite overwrite);
    const EntryModel& model() const { return *model_; }
    bool busy() const { return job_->running(); }

    // Asks for a password; `wrong` is true when the previous one was refused. Replaced by the tests.
    std::function<std::optional<QString>(bool wrong)> ask_password;

signals:
    // Emitted when opening, extracting or testing is over, whatever the outcome.
    void operationFinished();

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    enum class Mode { None, List, Extract, Test };

    void build_ui();
    void run_list();
    void run(const std::vector<std::string>& include);
    void on_listed(const ArchiveInfo& info);
    void on_list_failed(ErrorCode code, const QString& message);
    void on_finished(const Report& report);
    void on_progress(double fraction, const QString& current);
    void set_busy(Mode mode, const QString& status);
    void set_idle(const QString& status);
    void show_issues(const QStringList& lines);
    void update_actions();
    std::optional<QString> prompt_password(bool wrong);
    void choose_archive();
    void choose_destination();
    void about();

    EntryModel* model_ = nullptr;
    Job* job_ = nullptr;
    QSortFilterProxyModel* proxy_ = nullptr;

    QStackedWidget* pages_ = nullptr;
    QTableView* table_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QLineEdit* destination_ = nullptr;
    QComboBox* overwrite_ = nullptr;
    QPlainTextEdit* issues_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* open_folder_ = nullptr;

    QAction* open_action_ = nullptr;
    QAction* extract_action_ = nullptr;
    QAction* test_action_ = nullptr;
    QAction* cancel_action_ = nullptr;

    QString archive_;
    std::vector<std::string> passwords_;
    Mode mode_ = Mode::None;
    Report report_;    // result of the operation in progress, across password retries
    bool retrying_ = false;
};

} // namespace digzip::gui
