#pragma once

#include "io/stream.hpp"

#include <cstdint>
#include <memory>
#include <optional>

namespace digzip::codec {

// RAR 5 decompression. One instance serves every file of an archive, so that the files of a
// solid archive continue with the dictionary and tables left by the previous one.
class Rar5Unpacker {
public:
    Rar5Unpacker();
    Rar5Unpacker(const Rar5Unpacker&) = delete;
    Rar5Unpacker& operator=(const Rar5Unpacker&) = delete;
    ~Rar5Unpacker();

    // Starts the next file. Without `solid`, the decoder state is reset first. `dictionary` is
    // the size declared in the file header; `size` the decoded size, when known.
    void start(io::StreamPtr packed, std::optional<std::uint64_t> size, std::uint64_t dictionary, bool solid);

    // Decoded bytes of the current file; 0 at its end.
    std::size_t read(std::uint8_t* data, std::size_t size);

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace digzip::codec
