#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace digzip::io {

class InputStream {
public:
    InputStream() = default;
    InputStream(const InputStream&) = delete;
    InputStream& operator=(const InputStream&) = delete;
    virtual ~InputStream() = default;

    // Reads up to `size` bytes (size > 0). Returns 0 only at the end of the stream.
    virtual std::size_t read(std::uint8_t* data, std::size_t size) = 0;

    // Throws Error(Corrupt) if the stream ends first.
    void read_exact(std::uint8_t* data, std::size_t size);

    // Reads until `size` bytes or the end of the stream; returns the number of bytes read.
    std::size_t read_up_to(std::uint8_t* data, std::size_t size);

    // Discards up to `count` bytes; returns how many were discarded.
    std::uint64_t skip(std::uint64_t count);
};

using StreamPtr = std::unique_ptr<InputStream>;

// Random-access, read-only byte source. read_at() may be called from several threads.
class Source {
public:
    Source() = default;
    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;
    virtual ~Source() = default;

    virtual std::uint64_t size() const = 0;

    // Reads exactly `size` bytes at `offset`; throws Error(Corrupt) past the end.
    virtual void read_at(std::uint64_t offset, std::uint8_t* data, std::size_t size) const = 0;
};

using SourcePtr = std::shared_ptr<const Source>;

SourcePtr open_file(const std::filesystem::path& path);
SourcePtr memory_source(std::vector<std::uint8_t> bytes);

// Buffered sequential view of the bytes [offset, offset + length) of a source.
class SourceStream final : public InputStream {
public:
    SourceStream(SourcePtr source, std::uint64_t offset, std::uint64_t length);

    std::size_t read(std::uint8_t* data, std::size_t size) override;

private:
    SourcePtr source_;
    std::uint64_t next_;
    std::uint64_t end_;
    std::vector<std::uint8_t> buffer_;
    std::size_t pos_ = 0;
    std::size_t filled_ = 0;
};

// Exactly `length` bytes of an upstream stream it does not own. Throws Error(Corrupt) if the
// upstream ends early.
class SliceStream final : public InputStream {
public:
    SliceStream(InputStream& upstream, std::uint64_t length) : upstream_(upstream), remaining_(length) {}

    std::size_t read(std::uint8_t* data, std::size_t size) override;
    std::uint64_t remaining() const { return remaining_; }

private:
    InputStream& upstream_;
    std::uint64_t remaining_;
};

// Bytes already read while sniffing the format, followed by the rest of the upstream.
class PrefixedStream final : public InputStream {
public:
    PrefixedStream(std::vector<std::uint8_t> prefix, StreamPtr upstream)
        : prefix_(std::move(prefix)), upstream_(std::move(upstream))
    {
    }

    std::size_t read(std::uint8_t* data, std::size_t size) override;

private:
    std::vector<std::uint8_t> prefix_;
    std::size_t pos_ = 0;
    StreamPtr upstream_;
};

} // namespace digzip::io
