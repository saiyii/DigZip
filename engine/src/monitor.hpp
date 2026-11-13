#pragma once

#include "io/stream.hpp"

#include <digzip/archive.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

namespace digzip {

// Follows one operation on one archive: enforces Limits, reports progress, honors cancellation.
class Monitor {
public:
    // `archive_read` counts the bytes read from the archive (see io::count_reads).
    Monitor(const Options& options, std::uint64_t archive_size,
            std::shared_ptr<const std::atomic<std::uint64_t>> archive_read = nullptr);

    const Limits& limits() const { return options_.limits; }
    const std::vector<std::string>& passwords() const { return options_.passwords; }

    void add_entry(const std::string& path);
    void add_bytes(std::uint64_t count);
    void finish();

    // Throws Error(Cancelled) if cancellation was requested.
    void check_cancel() const;

private:
    void report();

    const Options& options_;
    std::uint64_t ratio_cap_ = 0; // 0 when the ratio is not limited
    std::shared_ptr<const std::atomic<std::uint64_t>> archive_read_;
    Progress progress_;
    std::chrono::steady_clock::time_point last_report_{};
};

// Charges every byte read through it to a Monitor.
class MonitoredStream final : public io::InputStream {
public:
    MonitoredStream(io::StreamPtr upstream, Monitor& monitor) : upstream_(std::move(upstream)), monitor_(monitor) {}

    std::size_t read(std::uint8_t* data, std::size_t size) override;

private:
    io::StreamPtr upstream_;
    Monitor& monitor_;
};

} // namespace digzip
