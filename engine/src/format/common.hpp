#pragma once

#include "io/stream.hpp"

#include <digzip/entry.hpp>

#include <cstdint>
#include <optional>

namespace digzip::format {

inline std::uint16_t le16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

inline std::uint32_t le32(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

inline std::uint64_t le64(const std::uint8_t* p)
{
    return le32(p) | (static_cast<std::uint64_t>(le32(p + 4)) << 32);
}

// Windows FILETIME: 100 ns intervals since 1601-01-01 UTC.
inline Timestamp from_filetime(std::uint64_t ticks)
{
    constexpr std::int64_t kEpochOffset = 11'644'473'600;
    return {static_cast<std::int64_t>(ticks / 10'000'000) - kEpochOffset,
            static_cast<std::uint32_t>((ticks % 10'000'000) * 100)};
}

// Verifies that an entry decodes to exactly `size` bytes with the given CRC-32 (when known).
// Throws Error(Corrupt) on a size mismatch and Error(Checksum) on a CRC mismatch.
class CheckedStream final : public io::InputStream {
public:
    CheckedStream(io::StreamPtr upstream, std::uint64_t size, std::optional<std::uint32_t> crc)
        : upstream_(std::move(upstream)), expected_size_(size), expected_crc_(crc)
    {
    }

    std::size_t read(std::uint8_t* data, std::size_t size) override;

private:
    io::StreamPtr upstream_;
    std::uint64_t expected_size_;
    std::optional<std::uint32_t> expected_crc_;
    std::uint64_t produced_ = 0;
    std::uint32_t crc_ = 0;
    bool done_ = false;
};

// Remembers the first failure of a shared stream (a solid block) and refuses later reads,
// so that a decoder is never used again after reporting an error.
class GuardedStream final : public io::InputStream {
public:
    explicit GuardedStream(io::StreamPtr upstream) : upstream_(std::move(upstream)) {}

    std::size_t read(std::uint8_t* data, std::size_t size) override;

private:
    io::StreamPtr upstream_;
    bool failed_ = false;
};

// A stream with no data, for empty files.
class EmptyStream final : public io::InputStream {
public:
    std::size_t read(std::uint8_t*, std::size_t) override { return 0; }
};

} // namespace digzip::format
