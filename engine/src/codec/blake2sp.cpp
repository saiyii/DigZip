// BLAKE2s and its 8-way tree mode BLAKE2sp, written for DigZip from the BLAKE2 specification
// (RFC 7693 for BLAKE2s, blake2.net for the parallel mode).

#include "codec/blake2sp.hpp"

#include <algorithm>
#include <cstring>

namespace digzip::codec {

namespace {

constexpr std::uint32_t kIv[8] = {0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
                                  0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19};

constexpr std::uint8_t kSigma[10][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4}, {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13}, {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11}, {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5}, {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
};

constexpr std::size_t kBlock = 64;
constexpr std::size_t kLeaves = 8;

std::uint32_t rotr(std::uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

std::uint32_t load32(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

} // namespace

// Parameter block: digest length 32, no key, fanout 8, depth 2, leaf length 0, node offset,
// node depth, inner length 32.
void Blake2sp::init(State& s, std::uint32_t node_offset, std::uint8_t node_depth, bool last_node)
{
    std::memset(&s, 0, sizeof s);
    std::uint8_t param[32] = {};
    param[0] = 32;
    param[2] = kLeaves;
    param[3] = 2;
    param[8] = static_cast<std::uint8_t>(node_offset);
    param[9] = static_cast<std::uint8_t>(node_offset >> 8);
    param[10] = static_cast<std::uint8_t>(node_offset >> 16);
    param[11] = static_cast<std::uint8_t>(node_offset >> 24);
    param[14] = node_depth;
    param[15] = 32;
    for (int i = 0; i < 8; ++i) {
        s.h[i] = kIv[i] ^ load32(param + 4 * i);
    }
    s.last_node = last_node;
}

void Blake2sp::compress(State& s, const std::uint8_t* block)
{
    std::uint32_t m[16];
    for (int i = 0; i < 16; ++i) {
        m[i] = load32(block + 4 * i);
    }
    std::uint32_t v[16];
    for (int i = 0; i < 8; ++i) {
        v[i] = s.h[i];
        v[i + 8] = kIv[i];
    }
    v[12] ^= s.t[0];
    v[13] ^= s.t[1];
    v[14] ^= s.f[0];
    v[15] ^= s.f[1];

    const auto g = [&](int r, int i, std::uint32_t& a, std::uint32_t& b, std::uint32_t& c, std::uint32_t& d) {
        a = a + b + m[kSigma[r][2 * i]];
        d = rotr(d ^ a, 16);
        c = c + d;
        b = rotr(b ^ c, 12);
        a = a + b + m[kSigma[r][2 * i + 1]];
        d = rotr(d ^ a, 8);
        c = c + d;
        b = rotr(b ^ c, 7);
    };
    for (int r = 0; r < 10; ++r) {
        g(r, 0, v[0], v[4], v[8], v[12]);
        g(r, 1, v[1], v[5], v[9], v[13]);
        g(r, 2, v[2], v[6], v[10], v[14]);
        g(r, 3, v[3], v[7], v[11], v[15]);
        g(r, 4, v[0], v[5], v[10], v[15]);
        g(r, 5, v[1], v[6], v[11], v[12]);
        g(r, 6, v[2], v[7], v[8], v[13]);
        g(r, 7, v[3], v[4], v[9], v[14]);
    }
    for (int i = 0; i < 8; ++i) {
        s.h[i] ^= v[i] ^ v[i + 8];
    }
}

// The last block is kept in the buffer: it must be compressed with the final flags.
void Blake2sp::update(State& s, const std::uint8_t* data, std::size_t size)
{
    while (size > 0) {
        if (s.length == kBlock) {
            s.t[0] += kBlock;
            if (s.t[0] < kBlock) {
                ++s.t[1];
            }
            compress(s, s.buffer);
            s.length = 0;
        }
        const std::size_t n = std::min(size, kBlock - s.length);
        std::memcpy(s.buffer + s.length, data, n);
        s.length += n;
        data += n;
        size -= n;
    }
}

std::array<std::uint8_t, 32> Blake2sp::finish(State& s)
{
    const auto length = static_cast<std::uint32_t>(s.length);
    s.t[0] += length;
    if (s.t[0] < length) {
        ++s.t[1];
    }
    s.f[0] = 0xFFFFFFFF;
    if (s.last_node) {
        s.f[1] = 0xFFFFFFFF;
    }
    std::memset(s.buffer + s.length, 0, kBlock - s.length);
    compress(s, s.buffer);
    std::array<std::uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i) {
        for (int k = 0; k < 4; ++k) {
            out[static_cast<std::size_t>(4 * i + k)] = static_cast<std::uint8_t>(s.h[i] >> (8 * k));
        }
    }
    return out;
}

Blake2sp::Blake2sp()
{
    init(root_, 0, 1, true);
    for (std::uint32_t i = 0; i < kLeaves; ++i) {
        init(leaves_[i], i, 0, i == kLeaves - 1);
    }
}

// Input is dealt to the leaves in 64-byte blocks, round-robin.
void Blake2sp::update(const std::uint8_t* data, std::size_t size)
{
    while (size > 0) {
        const std::size_t n = std::min(size, sizeof buffer_ - length_);
        std::memcpy(buffer_ + length_, data, n);
        length_ += n;
        data += n;
        size -= n;
        if (length_ == sizeof buffer_ && size > 0) {
            for (std::size_t i = 0; i < kLeaves; ++i) {
                update(leaves_[i], buffer_ + i * kBlock, kBlock);
            }
            length_ = 0;
        }
    }
}

std::array<std::uint8_t, 32> Blake2sp::finish()
{
    for (std::size_t i = 0; i < kLeaves; ++i) {
        if (length_ > i * kBlock) {
            update(leaves_[i], buffer_ + i * kBlock, std::min(kBlock, length_ - i * kBlock));
        }
        const std::array<std::uint8_t, 32> leaf = finish(leaves_[i]);
        update(root_, leaf.data(), leaf.size());
    }
    return finish(root_);
}

} // namespace digzip::codec
