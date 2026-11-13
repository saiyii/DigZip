#include "codec/decoders.hpp"
#include "codec/input_buffer.hpp"

#include <lzma.h>

#include <string>

namespace digzip::codec {

namespace {

class XzStream final : public io::InputStream {
public:
    XzStream(io::StreamPtr upstream, std::uint64_t memory_limit) : in_(std::move(upstream))
    {
        const std::uint64_t limit = memory_limit == 0 ? UINT64_MAX : memory_limit;
        if (lzma_stream_decoder(&lz_, limit, LZMA_CONCATENATED) != LZMA_OK) {
            throw Error(ErrorCode::Io, "xz: cannot allocate the decoder");
        }
    }

    ~XzStream() override { lzma_end(&lz_); }

    std::size_t read(std::uint8_t* data, std::size_t size) override
    {
        if (done_ || size == 0) {
            return 0;
        }
        lz_.next_out = data;
        lz_.avail_out = size;

        while (lz_.avail_out == size) {
            const bool more = in_.fill();
            lz_.next_in = in_.data();
            lz_.avail_in = in_.available();
            const std::size_t before = lz_.avail_in;

            const lzma_ret ret = lzma_code(&lz_, more ? LZMA_RUN : LZMA_FINISH);
            in_.consume(before - lz_.avail_in);

            if (ret == LZMA_STREAM_END) {
                done_ = true;
                break;
            }
            if (ret != LZMA_OK) {
                fail(ret);
            }
        }
        return size - lz_.avail_out;
    }

private:
    [[noreturn]] void fail(lzma_ret ret)
    {
        switch (ret) {
        case LZMA_MEMLIMIT_ERROR:
            throw Error(ErrorCode::LimitExceeded,
                        "xz: the stream needs " + std::to_string(lzma_memusage(&lz_) >> 20) +
                            " MiB of memory (limit: max_memory)");
        case LZMA_MEM_ERROR:
            throw Error(ErrorCode::Io, "xz: out of memory");
        case LZMA_OPTIONS_ERROR:
            throw Error(ErrorCode::Unsupported, "xz: unsupported options");
        case LZMA_BUF_ERROR:
            throw Error(ErrorCode::Corrupt, "xz: unexpected end of compressed data");
        case LZMA_DATA_ERROR:
            throw Error(ErrorCode::Checksum, "xz: integrity check failed, the data is corrupt");
        default:
            throw Error(ErrorCode::Corrupt, "xz: invalid compressed data");
        }
    }

    InputBuffer in_;
    lzma_stream lz_ = LZMA_STREAM_INIT;
    bool done_ = false;
};

} // namespace

io::StreamPtr decode_xz(io::StreamPtr upstream, std::uint64_t memory_limit)
{
    return std::make_unique<XzStream>(std::move(upstream), memory_limit);
}

} // namespace digzip::codec
