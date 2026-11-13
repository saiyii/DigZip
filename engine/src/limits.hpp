#pragma once

#include "io/stream.hpp"

#include <digzip/archive.hpp>

#include <cstdint>

namespace digzip {

// Counts entries and decoded bytes for one archive and enforces Limits.
class Budget {
public:
    Budget(const Limits& limits, std::uint64_t archive_size);

    const Limits& limits() const { return limits_; }

    void add_entry();
    void add_bytes(std::uint64_t count);

private:
    Limits limits_;
    std::uint64_t ratio_cap_ = 0; // 0 when the ratio is not limited
    std::uint64_t entries_ = 0;
    std::uint64_t bytes_ = 0;
};

// Charges every byte read through it to a Budget.
class BudgetStream final : public io::InputStream {
public:
    BudgetStream(io::StreamPtr upstream, Budget& budget) : upstream_(std::move(upstream)), budget_(budget) {}

    std::size_t read(std::uint8_t* data, std::size_t size) override;

private:
    io::StreamPtr upstream_;
    Budget& budget_;
};

} // namespace digzip
