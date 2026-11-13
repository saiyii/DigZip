#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace digzip::codec {

// BLAKE2sp (8-way parallel BLAKE2s, 256-bit digest), the file hash of RAR 5 archives.
class Blake2sp {
public:
    Blake2sp();
    void update(const std::uint8_t* data, std::size_t size);
    std::array<std::uint8_t, 32> finish();

private:
    struct State {
        std::uint32_t h[8];
        std::uint32_t t[2];
        std::uint32_t f[2];
        std::uint8_t buffer[64];
        std::size_t length;
        bool last_node;
    };

    static void init(State& s, std::uint32_t node_offset, std::uint8_t node_depth, bool last_node);
    static void compress(State& s, const std::uint8_t* block);
    static void update(State& s, const std::uint8_t* data, std::size_t size);
    static std::array<std::uint8_t, 32> finish(State& s);

    State root_;
    State leaves_[8];
    std::uint8_t buffer_[8 * 64];
    std::size_t length_ = 0;
};

} // namespace digzip::codec
