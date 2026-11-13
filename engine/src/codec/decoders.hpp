#pragma once

#include "io/stream.hpp"

#include <digzip/entry.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace digzip::codec {

struct GzipHeader {
    std::string name; // FNAME field, Latin-1 converted to UTF-8; empty if absent
    std::optional<Timestamp> mtime;
};

// Gzip file (RFC 1952), all members concatenated. CRC-32 and size are checked per member.
class GzipStream final : public io::InputStream {
public:
    explicit GzipStream(io::StreamPtr upstream);
    ~GzipStream() override;

    const GzipHeader& header() const { return header_; }
    std::size_t read(std::uint8_t* data, std::size_t size) override;

private:
    struct State;
    std::unique_ptr<State> state_;
    GzipHeader header_;
};

// Concatenated bzip2 streams.
io::StreamPtr decode_bzip2(io::StreamPtr upstream);

// Concatenated .xz streams. `memory_limit` bounds the decoder allocation.
io::StreamPtr decode_xz(io::StreamPtr upstream, std::uint64_t memory_limit);

// Zstandard frames. `memory_limit` bounds the window size.
io::StreamPtr decode_zstd(io::StreamPtr upstream, std::uint64_t memory_limit);

} // namespace digzip::codec
