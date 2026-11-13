#include "codec/decoders.hpp"
#include "io/stream.hpp"
#include "support/fixtures.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <zlib.h>

using namespace digzip;

namespace {

std::vector<std::uint8_t> deflate_with_zlib(const std::vector<std::uint8_t>& data, int level, int strategy)
{
    z_stream z{};
    REQUIRE(deflateInit2(&z, level, Z_DEFLATED, -MAX_WBITS, 8, strategy) == Z_OK);
    std::vector<std::uint8_t> out(deflateBound(&z, static_cast<uLong>(data.size())));
    z.next_in = const_cast<Bytef*>(data.data());
    z.avail_in = static_cast<uInt>(data.size());
    z.next_out = out.data();
    z.avail_out = static_cast<uInt>(out.size());
    REQUIRE(deflate(&z, Z_FINISH) == Z_STREAM_END);
    out.resize(z.total_out);
    deflateEnd(&z);
    return out;
}

io::StreamPtr memory_stream(std::vector<std::uint8_t> bytes)
{
    const std::uint64_t size = bytes.size();
    return std::make_unique<io::SourceStream>(io::memory_source(std::move(bytes)), 0, size);
}

std::vector<std::uint8_t> read_all(io::InputStream& stream, std::size_t chunk)
{
    std::vector<std::uint8_t> out;
    std::vector<std::uint8_t> buffer(chunk);
    for (std::size_t n; (n = stream.read(buffer.data(), buffer.size())) > 0;) {
        out.insert(out.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(n));
    }
    return out;
}

std::vector<std::uint8_t> sample_data()
{
    std::vector<std::uint8_t> data = fixtures::lcg_bytes(70'000);
    const std::string text = "the quick brown fox jumps over the lazy dog ";
    for (int i = 0; i < 3000; ++i) {
        data.insert(data.end(), text.begin(), text.end());
    }
    data.insert(data.end(), 40'000, 'z');
    return data;
}

} // namespace

TEST_CASE("DigZip's inflater matches zlib on stored, fixed and dynamic blocks")
{
    const auto [level, strategy] = GENERATE(table<int, int>({
        {0, Z_DEFAULT_STRATEGY},
        {1, Z_FIXED},
        {6, Z_DEFAULT_STRATEGY},
        {9, Z_DEFAULT_STRATEGY},
        {9, Z_RLE},
        {9, Z_HUFFMAN_ONLY},
    }));
    const auto chunk = static_cast<std::size_t>(GENERATE(1, 7, 4096, 1 << 20));
    CAPTURE(level, strategy, chunk);

    const auto data = sample_data();
    auto stream = codec::decode_inflate(memory_stream(deflate_with_zlib(data, level, strategy)), false);
    CHECK(read_all(*stream, chunk) == data);
}

TEST_CASE("DigZip's inflater rejects damaged input without crashing")
{
    const auto data = sample_data();
    auto compressed = deflate_with_zlib(data, 9, Z_DEFAULT_STRATEGY);

    auto truncated = compressed;
    truncated.resize(truncated.size() / 2);
    auto stream = codec::decode_inflate(memory_stream(truncated), false);
    CHECK_THROWS_AS(read_all(*stream, 4096), Error);

    for (std::size_t i = 0; i < compressed.size(); i += 997) {
        auto damaged = compressed;
        damaged[i] ^= 0x5A;
        auto decoder = codec::decode_inflate(memory_stream(damaged), false);
        try {
            const auto out = read_all(*decoder, 4096);
            CHECK(out.size() <= data.size() + 70'000);
        } catch (const Error& e) {
            CHECK(e.code() == ErrorCode::Corrupt);
        }
    }

    std::vector<std::uint8_t> invalid_block = {0x07}; // final block of reserved type 3
    auto bad = codec::decode_inflate(memory_stream(invalid_block), false);
    CHECK_THROWS_AS(read_all(*bad, 16), Error);
}
