// Fuzzing of the decoders written for DigZip, fed directly (without a container around them).
// The first byte chooses the decoder; the following bytes are its parameters and input.

#include "codec/decoders.hpp"
#include "codec/encryption.hpp"
#include "codec/rar5.hpp"
#include "io/stream.hpp"

#include <digzip/error.hpp>

#include <cstdint>
#include <vector>

namespace {

using digzip::io::StreamPtr;

constexpr std::uint64_t kMaxOutput = 4u << 20;
constexpr std::uint64_t kMaxMemory = 16u << 20;

StreamPtr stream_of(const std::uint8_t* data, std::size_t size)
{
    auto source = digzip::io::memory_source(std::vector<std::uint8_t>(data, data + size));
    return std::make_unique<digzip::io::SourceStream>(source, 0, size);
}

class RarStream final : public digzip::io::InputStream {
public:
    explicit RarStream(digzip::codec::Rar5Unpacker& unpacker) : unpacker_(unpacker) {}
    std::size_t read(std::uint8_t* data, std::size_t size) override { return unpacker_.read(data, size); }

private:
    digzip::codec::Rar5Unpacker& unpacker_;
};

void drain(digzip::io::InputStream& stream)
{
    std::vector<std::uint8_t> buffer(1 << 14);
    std::uint64_t total = 0;
    for (std::size_t n; total < kMaxOutput && (n = stream.read(buffer.data(), buffer.size())) > 0;) {
        total += n;
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    using namespace digzip;
    if (size < 8) {
        return 0;
    }
    const std::uint8_t selector = data[0];
    data += 1;
    size -= 1;
    try {
        StreamPtr decoded;
        codec::Rar5Unpacker rar;
        switch (selector % 7) {
        case 0:
            decoded = codec::decode_inflate(stream_of(data, size), true);
            break;
        case 1:
            decoded = codec::decode_inflate(stream_of(data, size), false);
            break;
        case 2: {
            // Order and memory from the input, memory kept small.
            std::vector<std::uint8_t> props(data, data + 5);
            props[1] = 0;
            props[2] = static_cast<std::uint8_t>(props[2] & 0x3F);
            props[3] = 0;
            props[4] = 0;
            decoded = codec::decode_ppmd7(stream_of(data + 5, size - 5), props, kMaxOutput, kMaxMemory);
            break;
        }
        case 3:
            decoded = codec::decode_ppmd8_zip(stream_of(data, size), kMaxOutput, kMaxMemory);
            break;
        case 4: {
            // Four inputs: the first bytes give the sizes of the call and jump streams.
            const std::size_t call = std::min<std::size_t>(data[0] * 4u, size - 2);
            const std::size_t jump = std::min<std::size_t>(data[1] * 4u, size - 2 - call);
            const std::size_t rest = size - 2 - call - jump;
            const std::uint8_t* p = data + 2;
            std::vector<StreamPtr> inputs;
            inputs.push_back(stream_of(p + call + jump, rest / 2));
            inputs.push_back(stream_of(p, call));
            inputs.push_back(stream_of(p + call, jump));
            inputs.push_back(stream_of(p + call + jump + rest / 2, rest - rest / 2));
            decoded = codec::decode_bcj2(std::move(inputs), kMaxOutput);
            break;
        }
        case 5:
            rar.start(stream_of(data, size), kMaxOutput, 1u << 17, false);
            decoded = std::make_unique<RarStream>(rar);
            break;
        default:
            decoded = codec::decrypt_zipcrypto(stream_of(data, size), "secret", data[11 < size ? 11 : 0]);
            break;
        }
        drain(*decoded);
    } catch (const Error&) {
    }
    return 0;
}
