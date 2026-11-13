#include "codec/decoders.hpp"
#include "codec/input_buffer.hpp"

#include <zstd.h>

#include <algorithm>

namespace digzip::codec {

namespace {

// Largest window (log2 of its size) that fits in `memory_limit` bytes.
int window_log_for(std::uint64_t memory_limit)
{
    const ZSTD_bounds bounds = ZSTD_dParam_getBounds(ZSTD_d_windowLogMax);
    if (memory_limit == 0) {
        return bounds.upperBound;
    }
    int log = bounds.lowerBound;
    while (log < bounds.upperBound && (std::uint64_t{1} << (log + 1)) <= memory_limit) {
        ++log;
    }
    return log;
}

class ZstdStream final : public io::InputStream {
public:
    ZstdStream(io::StreamPtr upstream, std::uint64_t memory_limit)
        : in_(std::move(upstream), ZSTD_DStreamInSize()), ctx_(ZSTD_createDCtx())
    {
        if (ctx_ == nullptr) {
            throw Error(ErrorCode::Io, "zstd: cannot allocate the decoder");
        }
        ZSTD_DCtx_setParameter(ctx_, ZSTD_d_windowLogMax, window_log_for(memory_limit));
    }

    ~ZstdStream() override { ZSTD_freeDCtx(ctx_); }

    std::size_t read(std::uint8_t* data, std::size_t size) override
    {
        if (done_ || size == 0) {
            return 0;
        }
        ZSTD_outBuffer out{data, size, 0};
        for (;;) {
            const bool more = in_.fill();
            if (!more && started_ && pending_ == 0) {
                done_ = true; // the last frame ended exactly at the end of the input
                return 0;
            }
            ZSTD_inBuffer input{in_.data(), in_.available(), 0};
            const std::size_t ret = ZSTD_decompressStream(ctx_, &out, &input);
            if (ZSTD_isError(ret)) {
                fail(ret);
            }
            in_.consume(input.pos);
            pending_ = ret;
            started_ = true;
            if (out.pos > 0) {
                return out.pos;
            }
            if (!more) {
                throw Error(ErrorCode::Corrupt, "zstd: unexpected end of compressed data");
            }
        }
    }

private:
    [[noreturn]] static void fail(std::size_t ret)
    {
        switch (ZSTD_getErrorCode(ret)) {
        case ZSTD_error_frameParameter_windowTooLarge:
            throw Error(ErrorCode::LimitExceeded, "zstd: the frame window exceeds the memory limit (limit: max_memory)");
        case ZSTD_error_checksum_wrong:
            throw Error(ErrorCode::Checksum, "zstd: checksum mismatch, the data is corrupt");
        case ZSTD_error_memory_allocation:
            throw Error(ErrorCode::Io, "zstd: out of memory");
        default:
            throw Error(ErrorCode::Corrupt, std::string("zstd: ") + ZSTD_getErrorName(ret));
        }
    }

    InputBuffer in_;
    ZSTD_DCtx* ctx_;
    std::size_t pending_ = 0; // non-zero while a frame is incomplete or not fully flushed
    bool started_ = false;
    bool done_ = false;
};

} // namespace

io::StreamPtr decode_zstd(io::StreamPtr upstream, std::uint64_t memory_limit)
{
    return std::make_unique<ZstdStream>(std::move(upstream), memory_limit);
}

} // namespace digzip::codec
