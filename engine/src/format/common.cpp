#include "format/common.hpp"

#include <digzip/error.hpp>

#include <zlib.h>

#include <algorithm>

namespace digzip::format {

std::size_t CheckedStream::read(std::uint8_t* data, std::size_t size)
{
    if (done_ || size == 0) {
        return 0;
    }
    const std::uint64_t remaining = expected_size_ - produced_;
    if (remaining == 0) {
        std::uint8_t extra = 0;
        if (upstream_->read(&extra, 1) != 0) {
            throw Error(ErrorCode::Corrupt, "the entry holds more data than declared");
        }
        if (expected_crc_ && crc_ != *expected_crc_) {
            throw Error(ErrorCode::Checksum, "CRC-32 mismatch, the entry is corrupt");
        }
        done_ = true;
        return 0;
    }
    const std::size_t n = upstream_->read(data, static_cast<std::size_t>(std::min<std::uint64_t>(size, remaining)));
    if (n == 0) {
        throw Error(ErrorCode::Corrupt, "the entry holds less data than declared");
    }
    crc_ = static_cast<std::uint32_t>(crc32(crc_, data, static_cast<uInt>(n)));
    produced_ += n;
    return n;
}

std::size_t GuardedStream::read(std::uint8_t* data, std::size_t size)
{
    if (failed_) {
        throw Error(ErrorCode::Corrupt, "an earlier error damaged this solid block");
    }
    try {
        return upstream_->read(data, size);
    } catch (...) {
        failed_ = true;
        throw;
    }
}

} // namespace digzip::format
