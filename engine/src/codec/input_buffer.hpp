#pragma once

#include "io/stream.hpp"

#include <digzip/error.hpp>

#include <cstring>
#include <vector>

namespace digzip::codec {

// Input side of a decoder: lets the codec library consume bytes in place and lets
// container code peek at headers and trailers that sit between compressed streams.
class InputBuffer {
public:
    explicit InputBuffer(io::StreamPtr upstream, std::size_t capacity = 1 << 16)
        : upstream_(std::move(upstream)), buffer_(capacity)
    {
    }

    const std::uint8_t* data() const { return buffer_.data() + pos_; }
    std::size_t available() const { return end_ - pos_; }
    void consume(std::size_t count) { pos_ += count; }

    // Reads more data when the buffer is empty. Returns false at the end of the upstream.
    bool fill() { return available() > 0 || ensure(1); }

    // True once the upstream has ended and every buffered byte was consumed.
    bool exhausted() { return !fill(); }

    // Makes at least `count` bytes available, if the upstream has them.
    bool ensure(std::size_t count)
    {
        if (available() >= count) {
            return true;
        }
        if (pos_ > 0) {
            std::memmove(buffer_.data(), data(), available());
            end_ -= pos_;
            pos_ = 0;
        }
        while (end_ < count && !upstream_ended_) {
            const std::size_t n = upstream_->read(buffer_.data() + end_, buffer_.size() - end_);
            if (n == 0) {
                upstream_ended_ = true;
            }
            end_ += n;
        }
        return available() >= count;
    }

    std::uint8_t read_byte()
    {
        if (!ensure(1)) {
            throw Error(ErrorCode::Corrupt, "unexpected end of compressed data");
        }
        return buffer_[pos_++];
    }

    std::uint32_t read_le32()
    {
        std::uint32_t value = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            value |= static_cast<std::uint32_t>(read_byte()) << shift;
        }
        return value;
    }

private:
    io::StreamPtr upstream_;
    std::vector<std::uint8_t> buffer_;
    std::size_t pos_ = 0;
    std::size_t end_ = 0;
    bool upstream_ended_ = false;
};

} // namespace digzip::codec
