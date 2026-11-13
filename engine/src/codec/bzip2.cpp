#include "codec/decoders.hpp"
#include "codec/input_buffer.hpp"

#include <algorithm>
#include <bzlib.h>
#include <climits>

namespace digzip::codec {

namespace {

class Bzip2Stream final : public io::InputStream {
public:
    explicit Bzip2Stream(io::StreamPtr upstream) : in_(std::move(upstream)) { start(); }

    ~Bzip2Stream() override
    {
        if (active_) {
            BZ2_bzDecompressEnd(&bz_);
        }
    }

    std::size_t read(std::uint8_t* data, std::size_t size) override
    {
        if (done_ || size == 0) {
            return 0;
        }
        const auto capacity = static_cast<unsigned>(std::min<std::size_t>(size, UINT_MAX));
        bz_.next_out = reinterpret_cast<char*>(data);
        bz_.avail_out = capacity;

        while (bz_.avail_out == capacity) {
            if (!in_.fill()) {
                throw Error(ErrorCode::Corrupt, "bzip2: unexpected end of compressed data");
            }
            const auto input = static_cast<unsigned>(std::min<std::size_t>(in_.available(), UINT_MAX));
            bz_.next_in = const_cast<char*>(reinterpret_cast<const char*>(in_.data()));
            bz_.avail_in = input;

            const int ret = BZ2_bzDecompress(&bz_);
            in_.consume(input - bz_.avail_in);

            if (ret == BZ_STREAM_END) {
                BZ2_bzDecompressEnd(&bz_);
                active_ = false;
                if (!next_stream_follows()) {
                    done_ = true;
                    break;
                }
                char* const next_out = bz_.next_out;
                const unsigned avail_out = bz_.avail_out;
                start();
                bz_.next_out = next_out;
                bz_.avail_out = avail_out;
            } else if (ret == BZ_MEM_ERROR) {
                throw Error(ErrorCode::Io, "bzip2: out of memory");
            } else if (ret != BZ_OK) {
                throw Error(ErrorCode::Corrupt, "bzip2: invalid compressed data");
            }
        }
        return capacity - bz_.avail_out;
    }

private:
    void start()
    {
        bz_ = bz_stream{};
        if (BZ2_bzDecompressInit(&bz_, 0, 0) != BZ_OK) {
            throw Error(ErrorCode::Io, "bzip2: cannot allocate the decoder");
        }
        active_ = true;
    }

    bool next_stream_follows()
    {
        return in_.ensure(4) && in_.data()[0] == 'B' && in_.data()[1] == 'Z' && in_.data()[2] == 'h' &&
               in_.data()[3] >= '1' && in_.data()[3] <= '9';
    }

    InputBuffer in_;
    bz_stream bz_{};
    bool active_ = false;
    bool done_ = false;
};

} // namespace

io::StreamPtr decode_bzip2(io::StreamPtr upstream)
{
    return std::make_unique<Bzip2Stream>(std::move(upstream));
}

} // namespace digzip::codec
