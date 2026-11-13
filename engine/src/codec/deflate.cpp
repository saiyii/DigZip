#include "codec/decoders.hpp"
#include "codec/input_buffer.hpp"

#include <algorithm>
#include <climits>
#include <zlib.h>

namespace digzip::codec {

namespace {

class DeflateStream final : public io::InputStream {
public:
    explicit DeflateStream(io::StreamPtr upstream) : in_(std::move(upstream))
    {
        if (inflateInit2(&z_, -MAX_WBITS) != Z_OK) {
            throw Error(ErrorCode::Io, "deflate: cannot allocate the decoder");
        }
    }

    ~DeflateStream() override { inflateEnd(&z_); }

    std::size_t read(std::uint8_t* data, std::size_t size) override
    {
        if (done_ || size == 0) {
            return 0;
        }
        const auto capacity = static_cast<uInt>(std::min<std::size_t>(size, UINT_MAX));
        z_.next_out = data;
        z_.avail_out = capacity;

        while (z_.avail_out == capacity) {
            if (!in_.fill()) {
                throw Error(ErrorCode::Corrupt, "deflate: unexpected end of compressed data");
            }
            const auto input = static_cast<uInt>(std::min<std::size_t>(in_.available(), UINT_MAX));
            z_.next_in = const_cast<Bytef*>(in_.data());
            z_.avail_in = input;
            const int ret = inflate(&z_, Z_NO_FLUSH);
            in_.consume(input - z_.avail_in);
            if (ret == Z_STREAM_END) {
                done_ = true;
                break;
            }
            if (ret != Z_OK) {
                throw Error(ErrorCode::Corrupt,
                            std::string("deflate: ") + (z_.msg != nullptr ? z_.msg : "invalid compressed data"));
            }
        }
        return capacity - z_.avail_out;
    }

private:
    InputBuffer in_;
    z_stream z_{};
    bool done_ = false;
};

} // namespace

io::StreamPtr decode_deflate(io::StreamPtr upstream)
{
    return std::make_unique<DeflateStream>(std::move(upstream));
}

} // namespace digzip::codec
