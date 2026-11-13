#include "job.hpp"

#include <QMetaObject>

#include <functional>
#include <new>

namespace digzip::gui {

std::filesystem::path to_path(const QString& text)
{
    return std::filesystem::path(text.toStdU16String());
}

QString from_path(const std::filesystem::path& path)
{
    return QString::fromStdU16String(path.u16string());
}

namespace {

Report failed_report(ErrorCode code, const std::string& message)
{
    Report report;
    report.issues.push_back({Severity::Error, code, {}, message});
    return report;
}

} // namespace

Job::Job(QObject* parent) : QObject(parent) {}

Job::~Job()
{
    cancel_ = true;
    if (worker_.joinable()) {
        worker_.join();
    }
}

void Job::cancel()
{
    cancel_ = true;
}

// `work` runs on the worker thread with the prepared options and returns what to do, on the
// owner's thread, once it is over.
template <typename Opts, typename Work>
void Job::start(Opts options, Work work)
{
    if (worker_.joinable()) {
        worker_.join();
    }
    cancel_ = false;
    running_ = true;
    progress_pending_ = false;
    options.cancel = &cancel_;
    // Progress can be reported thousands of times per second: only the latest value is kept,
    // and a single delivery is queued at a time.
    options.on_progress = [this](const Progress& progress) {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        latest_ = progress;
        if (!progress_pending_) {
            progress_pending_ = true;
            QMetaObject::invokeMethod(this, [this] { deliver_progress(); }, Qt::QueuedConnection);
        }
    };
    worker_ = std::thread([this, options = std::move(options), work = std::move(work)] {
        std::function<void()> deliver = work(options);
        QMetaObject::invokeMethod(this, [this, deliver = std::move(deliver)] {
            running_ = false;
            deliver();
        }, Qt::QueuedConnection);
    });
}

void Job::deliver_progress()
{
    Progress progress;
    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        progress = latest_;
        progress_pending_ = false;
    }
    if (!running_) {
        return;
    }
    const double fraction = progress.archive_size > 0
                                ? static_cast<double>(progress.archive_read) / static_cast<double>(progress.archive_size)
                                : -1.0;
    emit this->progress(fraction, QString::fromUtf8(progress.current.c_str()));
}

void Job::list(const QString& archive, const Options& options)
{
    start(options, [this, path = to_path(archive)](const Options& prepared) -> std::function<void()> {
        try {
            ArchiveInfo info = digzip::list(path, prepared);
            return [this, info = std::move(info)] { emit listed(info); };
        } catch (const Error& e) {
            return [this, code = e.code(), message = QString::fromUtf8(e.what())] { emit listFailed(code, message); };
        } catch (const std::bad_alloc&) {
            return [this] { emit listFailed(ErrorCode::LimitExceeded, tr("Not enough memory to read this archive.")); };
        } catch (const std::exception& e) {
            return [this, message = QString::fromUtf8(e.what())] { emit listFailed(ErrorCode::Io, message); };
        }
    });
}

void Job::extract(const QString& archive, const QString& destination, const ExtractOptions& options)
{
    start(options, [this, path = to_path(archive), out = to_path(destination)](
                       const ExtractOptions& prepared) -> std::function<void()> {
        Report report;
        try {
            report = digzip::extract(path, out, prepared);
        } catch (const std::exception& e) {
            report = failed_report(ErrorCode::Io, e.what());
        }
        return [this, report = std::move(report)] { emit finished(report); };
    });
}

void Job::test(const QString& archive, const Options& options)
{
    start(options, [this, path = to_path(archive)](const Options& prepared) -> std::function<void()> {
        Report report;
        try {
            report = digzip::test(path, prepared);
        } catch (const std::exception& e) {
            report = failed_report(ErrorCode::Io, e.what());
        }
        return [this, report = std::move(report)] { emit finished(report); };
    });
}

} // namespace digzip::gui
