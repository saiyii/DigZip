#include "codec/decoders.hpp"
#include "codec/input_buffer.hpp"

#include <lzma.h>

#include <cstdlib>
#include <string>

namespace digzip::codec {

namespace {

class LzmaRawStream final : public io::InputStream {
public:
    LzmaRawStream(io::StreamPtr upstream, const std::vector<LzmaFilter>& chain, std::optional<std::uint64_t> size,
                  std::uint64_t memory_limit)
        : in_(std::move(upstream))
    {
        if (chain.empty() || chain.size() > LZMA_FILTERS_MAX) {
            throw Error(ErrorCode::Unsupported, "lzma: unsupported filter chain");
        }
        try {
            for (const LzmaFilter& f : chain) {
                lzma_filter filter{static_cast<lzma_vli>(f.id), nullptr};
                if (lzma_properties_decode(&filter, nullptr, f.properties.data(), f.properties.size()) != LZMA_OK) {
                    throw Error(ErrorCode::Unsupported, "lzma: unsupported or invalid filter properties");
                }
                filters_[count_++] = filter;
            }
            lzma_filter& last = filters_[count_ - 1];
            if (last.id == LZMA_FILTER_LZMA1 && size) {
                // LZMA1 often has no end marker: give the decoder the size so it knows where to stop.
                auto* options = static_cast<lzma_options_lzma*>(last.options);
                last.id = LZMA_FILTER_LZMA1EXT;
                options->ext_flags = LZMA_LZMA1EXT_ALLOW_EOPM;
                options->ext_size_low = static_cast<std::uint32_t>(*size);
                options->ext_size_high = static_cast<std::uint32_t>(*size >> 32);
            }
            filters_[count_] = {LZMA_VLI_UNKNOWN, nullptr};

            const std::uint64_t needed = lzma_raw_decoder_memusage(filters_);
            if (memory_limit != 0 && needed > memory_limit) {
                throw Error(ErrorCode::LimitExceeded, "lzma: the stream needs " + std::to_string(needed >> 20) +
                                                          " MiB of memory (limit: max_memory)");
            }
            if (lzma_raw_decoder(&lz_, filters_) != LZMA_OK) {
                throw Error(ErrorCode::Unsupported, "lzma: unsupported filter chain");
            }
        } catch (...) {
            release_options();
            throw;
        }
    }

    ~LzmaRawStream() override
    {
        lzma_end(&lz_);
        release_options();
    }

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
            if (ret == LZMA_BUF_ERROR || (!more && ret == LZMA_OK && lz_.avail_out == size)) {
                throw Error(ErrorCode::Corrupt, "lzma: unexpected end of compressed data");
            }
            if (ret == LZMA_MEM_ERROR) {
                throw Error(ErrorCode::Io, "lzma: out of memory");
            }
            if (ret != LZMA_OK) {
                throw Error(ErrorCode::Corrupt, "lzma: invalid compressed data");
            }
        }
        return size - lz_.avail_out;
    }

private:
    void release_options()
    {
        for (std::size_t i = 0; i < count_; ++i) {
            std::free(filters_[i].options);
            filters_[i].options = nullptr;
        }
        count_ = 0;
    }

    InputBuffer in_;
    lzma_stream lz_ = LZMA_STREAM_INIT;
    lzma_filter filters_[LZMA_FILTERS_MAX + 1] = {};
    std::size_t count_ = 0;
    bool done_ = false;
};

} // namespace

io::StreamPtr decode_lzma_raw(io::StreamPtr upstream, const std::vector<LzmaFilter>& chain,
                              std::optional<std::uint64_t> size, std::uint64_t memory_limit)
{
    return std::make_unique<LzmaRawStream>(std::move(upstream), chain, size, memory_limit);
}

} // namespace digzip::codec
