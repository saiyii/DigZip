#pragma once

#include <digzip/archive.hpp>

#include <QObject>
#include <QString>

#include <atomic>
#include <mutex>
#include <thread>

namespace digzip::gui {

std::filesystem::path to_path(const QString& text);
QString from_path(const std::filesystem::path& path);

// Runs one engine operation at a time on a worker thread. Results and progress are delivered
// through the signals, on the thread that owns the Job.
class Job : public QObject {
    Q_OBJECT

public:
    explicit Job(QObject* parent = nullptr);
    ~Job() override;

    bool running() const { return running_; }

    void list(const QString& archive, const Options& options);
    void extract(const QString& archive, const QString& destination, const ExtractOptions& options);
    void test(const QString& archive, const Options& options);
    void cancel();

signals:
    // `fraction` is in [0, 1], or negative while the total is unknown.
    void progress(double fraction, const QString& current);
    void listed(const digzip::ArchiveInfo& info);
    void listFailed(digzip::ErrorCode code, const QString& message);
    void finished(const digzip::Report& report);

private:
    template <typename Opts, typename Work>
    void start(Opts options, Work work);
    void deliver_progress();

    std::thread worker_;
    std::atomic<bool> cancel_{false};
    bool running_ = false;

    std::mutex progress_mutex_;
    Progress latest_;
    bool progress_pending_ = false;
};

} // namespace digzip::gui
