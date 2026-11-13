#include "codec/decoders.hpp"
#include "codec/input_buffer.hpp"
#include "text.hpp"

#include <algorithm>
#include <climits>
#include <zlib.h>

namespace digzip::codec {

namespace {

constexpr std::uint8_t kFlagHeaderCrc = 0x02;
constexpr std::uint8_t kFlagExtra = 0x04;
constexpr std::uint8_t kFlagName = 0x08;
constexpr std::uint8_t kFlagComment = 0x10;
constexpr std::uint8_t kFlagReserved = 0xE0;
constexpr std::size_t kMaxNameLength = 4096;

[[noreturn]] void corrupt(const std::string& what)
{
    throw Error(ErrorCode::Corrupt, "gzip: " + what);
}

} // namespace

struct GzipStream::State {
    explicit State(io::StreamPtr upstream) : in(std::move(upstream)) {}

    InputBuffer in;
    z_stream z{};
    bool z_ready = false;
    std::uint32_t crc = 0;
    std::uint32_t size = 0; // modulo 2^32, as stored in the trailer
    bool done = false;

    GzipHeader read_header()
    {
        GzipHeader header;
        if (in.read_byte() != 0x1F || in.read_byte() != 0x8B) {
            corrupt("bad signature");
        }
        if (in.read_byte() != 8) {
            throw Error(ErrorCode::Unsupported, "gzip: unknown compression method");
        }
        const std::uint8_t flags = in.read_byte();
        if (flags & kFlagReserved) {
            corrupt("reserved header flags are set");
        }
        const std::uint32_t mtime = in.read_le32();
        if (mtime != 0) {
            header.mtime = Timestamp{mtime, 0};
        }
        in.read_byte(); // extra flags
        in.read_byte(); // operating system
        if (flags & kFlagExtra) {
            std::uint32_t length = in.read_byte();
            length |= static_cast<std::uint32_t>(in.read_byte()) << 8;
            for (std::uint32_t i = 0; i < length; ++i) {
                in.read_byte();
            }
        }
        if (flags & kFlagName) {
            std::string name;
            for (std::uint8_t c = in.read_byte(); c != 0; c = in.read_byte()) {
                if (name.size() == kMaxNameLength) {
                    corrupt("file name field is too long");
                }
                name += static_cast<char>(c);
            }
            header.name = text::latin1_to_utf8(name);
        }
        if (flags & kFlagComment) {
            while (in.read_byte() != 0) {
            }
        }
        if (flags & kFlagHeaderCrc) {
            in.read_byte();
            in.read_byte();
        }
        return header;
    }

    // Checks the trailer of the member that just ended. Returns true if another member follows.
    bool finish_member()
    {
        if (in.read_le32() != crc) {
            throw Error(ErrorCode::Checksum, "gzip: CRC-32 mismatch, the data is corrupt");
        }
        if (in.read_le32() != size) {
            corrupt("size mismatch");
        }
        if (!in.ensure(2) || in.data()[0] != 0x1F || in.data()[1] != 0x8B) {
            return false; // end of file, or trailing data that is not another member
        }
        read_header();
        if (inflateReset(&z) != Z_OK) {
            corrupt("cannot reset the decoder");
        }
        crc = 0;
        size = 0;
        return true;
    }
};

GzipStream::GzipStream(io::StreamPtr upstream) : state_(std::make_unique<State>(std::move(upstream)))
{
    header_ = state_->read_header();
    if (inflateInit2(&state_->z, -MAX_WBITS) != Z_OK) {
        throw Error(ErrorCode::Io, "gzip: cannot allocate the decoder");
    }
    state_->z_ready = true;
}

GzipStream::~GzipStream()
{
    if (state_->z_ready) {
        inflateEnd(&state_->z);
    }
}

std::size_t GzipStream::read(std::uint8_t* data, std::size_t size)
{
    State& s = *state_;
    if (s.done || size == 0) {
        return 0;
    }
    const auto capacity = static_cast<uInt>(std::min<std::size_t>(size, UINT_MAX));
    s.z.next_out = data;
    s.z.avail_out = capacity;

    while (s.z.avail_out == capacity) {
        if (!s.in.fill()) {
            corrupt("unexpected end of compressed data");
        }
        const auto input = static_cast<uInt>(std::min<std::size_t>(s.in.available(), UINT_MAX));
        s.z.next_in = const_cast<Bytef*>(s.in.data());
        s.z.avail_in = input;
        Bytef* const out_start = s.z.next_out;

        const int ret = inflate(&s.z, Z_NO_FLUSH);

        s.in.consume(input - s.z.avail_in);
        const auto produced = static_cast<uInt>(s.z.next_out - out_start);
        s.crc = static_cast<std::uint32_t>(crc32(s.crc, out_start, produced));
        s.size += produced;

        if (ret == Z_STREAM_END) {
            if (!s.finish_member()) {
                s.done = true;
                break;
            }
        } else if (ret != Z_OK) {
            corrupt(s.z.msg != nullptr ? s.z.msg : "invalid compressed data");
        }
    }
    return capacity - s.z.avail_out;
}

} // namespace digzip::codec
