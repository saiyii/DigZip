#pragma once

#include "io/stream.hpp"

#include <digzip/entry.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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

// Raw deflate (RFC 1951), through zlib.
io::StreamPtr decode_deflate(io::StreamPtr upstream);

// DigZip's own inflater. Decodes Deflate64 ("enhanced deflate": 64 KiB window, longer matches)
// or, with deflate64 = false, plain deflate.
io::StreamPtr decode_inflate(io::StreamPtr upstream, bool deflate64);

// Concatenated bzip2 streams.
io::StreamPtr decode_bzip2(io::StreamPtr upstream);

// One filter of a raw liblzma chain: LZMA1, LZMA2, BCJ variants or Delta, with its 7z-style properties.
struct LzmaFilter {
    std::uint64_t id; // liblzma filter ID
    std::vector<std::uint8_t> properties;
};

// Raw chain without .xz framing, as stored in ZIP (method 14) and 7z. Filters are listed in
// encoding order: [BCJ, LZMA2] means BCJ ran first when compressing. `size` is the decoded size,
// required for LZMA1 data without an end marker.
io::StreamPtr decode_lzma_raw(io::StreamPtr upstream, const std::vector<LzmaFilter>& chain,
                              std::optional<std::uint64_t> size, std::uint64_t memory_limit);

// 7z BCJ2 x86 filter. Inputs: main, call, jump and range-coded streams; `size` is the output size.
io::StreamPtr decode_bcj2(std::vector<io::StreamPtr> inputs, std::uint64_t size);

// Concatenated .xz streams. `memory_limit` bounds the decoder allocation.
io::StreamPtr decode_xz(io::StreamPtr upstream, std::uint64_t memory_limit);

// Zstandard frames. `memory_limit` bounds the window size.
io::StreamPtr decode_zstd(io::StreamPtr upstream, std::uint64_t memory_limit);

} // namespace digzip::codec
