#include "limits.hpp"

#include <algorithm>
#include <limits>
#include <string>

namespace digzip {

namespace {

std::string mib(std::uint64_t bytes)
{
    return std::to_string(bytes >> 20) + " MiB";
}

} // namespace

Budget::Budget(const Limits& limits, std::uint64_t archive_size) : limits_(limits)
{
    if (limits_.max_ratio != 0) {
        const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
        const std::uint64_t scaled =
            archive_size > max / limits_.max_ratio ? max : archive_size * limits_.max_ratio;
        ratio_cap_ = std::max(scaled, limits_.ratio_threshold);
    }
}

void Budget::add_entry()
{
    ++entries_;
    if (limits_.max_entries != 0 && entries_ > limits_.max_entries) {
        throw Error(ErrorCode::LimitExceeded,
                    "the archive has more than " + std::to_string(limits_.max_entries) +
                        " entries (limit: max_entries)");
    }
}

void Budget::add_bytes(std::uint64_t count)
{
    bytes_ += count;
    if (limits_.max_total_size != 0 && bytes_ > limits_.max_total_size) {
        throw Error(ErrorCode::LimitExceeded, "the archive expands to more than " +
                                                  mib(limits_.max_total_size) +
                                                  " (limit: max_total_size)");
    }
    if (ratio_cap_ != 0 && bytes_ > ratio_cap_) {
        throw Error(ErrorCode::LimitExceeded,
                    "the archive expands more than " + std::to_string(limits_.max_ratio) +
                        " times its size, which looks like a decompression bomb (limit: max_ratio)");
    }
}

std::size_t BudgetStream::read(std::uint8_t* data, std::size_t size)
{
    const std::size_t n = upstream_->read(data, size);
    budget_.add_bytes(n);
    return n;
}

} // namespace digzip
