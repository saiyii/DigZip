#include "monitor.hpp"

#include <algorithm>
#include <limits>
#include <string>

namespace digzip {

namespace {

constexpr auto kReportInterval = std::chrono::milliseconds(100);

std::string mib(std::uint64_t bytes)
{
    return std::to_string(bytes >> 20) + " MiB";
}

} // namespace

Monitor::Monitor(const Options& options, std::uint64_t archive_size,
                 std::shared_ptr<const std::atomic<std::uint64_t>> archive_read)
    : options_(options), archive_read_(std::move(archive_read))
{
    const Limits& limits = options_.limits;
    if (limits.max_ratio != 0) {
        const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
        const std::uint64_t scaled = archive_size > max / limits.max_ratio ? max : archive_size * limits.max_ratio;
        ratio_cap_ = std::max(scaled, limits.ratio_threshold);
    }
    progress_.archive_size = archive_size;
}

void Monitor::check_cancel() const
{
    if (options_.cancel != nullptr && options_.cancel->load(std::memory_order_relaxed)) {
        throw Error(ErrorCode::Cancelled, "cancelled");
    }
}

void Monitor::add_entry(const std::string& path)
{
    check_cancel();
    ++progress_.entries;
    const std::uint64_t max_entries = options_.limits.max_entries;
    if (max_entries != 0 && progress_.entries > max_entries) {
        throw Error(ErrorCode::LimitExceeded,
                    "the archive has more than " + std::to_string(max_entries) + " entries (limit: max_entries)");
    }
    progress_.current = path;
    report();
}

void Monitor::add_bytes(std::uint64_t count)
{
    progress_.bytes += count;
    const Limits& limits = options_.limits;
    if (limits.max_total_size != 0 && progress_.bytes > limits.max_total_size) {
        throw Error(ErrorCode::LimitExceeded,
                    "the archive expands to more than " + mib(limits.max_total_size) + " (limit: max_total_size)");
    }
    if (ratio_cap_ != 0 && progress_.bytes > ratio_cap_) {
        throw Error(ErrorCode::LimitExceeded,
                    "the archive expands more than " + std::to_string(limits.max_ratio) +
                        " times its size, which looks like a decompression bomb (limit: max_ratio)");
    }
    check_cancel();
    if (options_.on_progress && std::chrono::steady_clock::now() - last_report_ >= kReportInterval) {
        report();
    }
}

void Monitor::finish()
{
    progress_.current.clear();
    report();
}

void Monitor::report()
{
    if (!options_.on_progress) {
        return;
    }
    if (archive_read_) {
        progress_.archive_read = std::min(archive_read_->load(std::memory_order_relaxed), progress_.archive_size);
    }
    last_report_ = std::chrono::steady_clock::now();
    options_.on_progress(progress_);
}

std::size_t MonitoredStream::read(std::uint8_t* data, std::size_t size)
{
    const std::size_t n = upstream_->read(data, size);
    monitor_.add_bytes(n);
    return n;
}

} // namespace digzip
