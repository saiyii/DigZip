#include "io/stream.hpp"

#include <digzip/error.hpp>

#include <algorithm>
#include <cstring>

namespace digzip::io {

namespace {

constexpr std::size_t kSourceBufferSize = 1 << 16;

class MemorySource final : public Source {
public:
    explicit MemorySource(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}

    std::uint64_t size() const override { return bytes_.size(); }

    void read_at(std::uint64_t offset, std::uint8_t* data, std::size_t size) const override
    {
        if (offset > bytes_.size() || size > bytes_.size() - offset) {
            throw Error(ErrorCode::Corrupt, "unexpected end of archive");
        }
        std::memcpy(data, bytes_.data() + offset, size);
    }

private:
    std::vector<std::uint8_t> bytes_;
};

class CountingSource final : public Source {
public:
    CountingSource(SourcePtr inner, std::shared_ptr<std::atomic<std::uint64_t>> counter)
        : inner_(std::move(inner)), counter_(std::move(counter))
    {
    }

    std::uint64_t size() const override { return inner_->size(); }

    void read_at(std::uint64_t offset, std::uint8_t* data, std::size_t size) const override
    {
        inner_->read_at(offset, data, size);
        counter_->fetch_add(size, std::memory_order_relaxed);
    }

private:
    SourcePtr inner_;
    std::shared_ptr<std::atomic<std::uint64_t>> counter_;
};

} // namespace

SourcePtr count_reads(SourcePtr source, std::shared_ptr<std::atomic<std::uint64_t>> counter)
{
    return std::make_shared<CountingSource>(std::move(source), std::move(counter));
}

void InputStream::read_exact(std::uint8_t* data, std::size_t size)
{
    if (read_up_to(data, size) != size) {
        throw Error(ErrorCode::Corrupt, "unexpected end of data");
    }
}

std::size_t InputStream::read_up_to(std::uint8_t* data, std::size_t size)
{
    std::size_t total = 0;
    while (total < size) {
        const std::size_t n = read(data + total, size - total);
        if (n == 0) {
            break;
        }
        total += n;
    }
    return total;
}

std::uint64_t InputStream::skip(std::uint64_t count)
{
    std::uint8_t buffer[16384];
    std::uint64_t skipped = 0;
    while (skipped < count) {
        const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(sizeof buffer, count - skipped));
        const std::size_t n = read(buffer, chunk);
        if (n == 0) {
            break;
        }
        skipped += n;
    }
    return skipped;
}

SourcePtr memory_source(std::vector<std::uint8_t> bytes)
{
    return std::make_shared<MemorySource>(std::move(bytes));
}

SourceStream::SourceStream(SourcePtr source, std::uint64_t offset, std::uint64_t length)
    : source_(std::move(source)), next_(offset), end_(offset)
{
    const std::uint64_t total = source_->size();
    if (offset > total || length > total - offset) {
        throw Error(ErrorCode::Corrupt, "data range lies outside the archive");
    }
    end_ = offset + length;
    buffer_.resize(static_cast<std::size_t>(std::min<std::uint64_t>(kSourceBufferSize, length)));
}

std::size_t SourceStream::read(std::uint8_t* data, std::size_t size)
{
    if (pos_ == filled_) {
        const std::uint64_t left = end_ - next_;
        if (left == 0 || size == 0) {
            return 0;
        }
        if (size >= buffer_.size()) {
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(size, left));
            source_->read_at(next_, data, n);
            next_ += n;
            return n;
        }
        filled_ = static_cast<std::size_t>(std::min<std::uint64_t>(buffer_.size(), left));
        source_->read_at(next_, buffer_.data(), filled_);
        next_ += filled_;
        pos_ = 0;
    }
    const std::size_t n = std::min(size, filled_ - pos_);
    std::memcpy(data, buffer_.data() + pos_, n);
    pos_ += n;
    return n;
}

std::size_t SliceStream::read(std::uint8_t* data, std::size_t size)
{
    if (remaining_ == 0 || size == 0) {
        return 0;
    }
    const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(size, remaining_));
    const std::size_t n = upstream_.read(data, wanted);
    if (n == 0) {
        throw Error(ErrorCode::Corrupt, "unexpected end of data");
    }
    remaining_ -= n;
    return n;
}

std::size_t PrefixedStream::read(std::uint8_t* data, std::size_t size)
{
    if (pos_ < prefix_.size()) {
        const std::size_t n = std::min(size, prefix_.size() - pos_);
        std::memcpy(data, prefix_.data() + pos_, n);
        pos_ += n;
        return n;
    }
    return upstream_->read(data, size);
}

} // namespace digzip::io
