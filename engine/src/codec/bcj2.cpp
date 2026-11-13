// BCJ2 decoder written for DigZip. BCJ2 is the 7z x86 branch converter that splits code into
// four streams: the main bytes, CALL targets, JUMP targets, and a range-coded stream telling, for
// each E8 / E9 / 0F 8x opcode, whether its operand was converted to an absolute address.

#include "codec/decoders.hpp"
#include "codec/input_buffer.hpp"

namespace digzip::codec {

namespace {

constexpr std::uint32_t kTopValue = 1u << 24;
constexpr int kBitModelBits = 11;
constexpr std::uint32_t kBitModelTotal = 1u << kBitModelBits;
constexpr int kMoveBits = 5;

bool is_jump(std::uint8_t previous, std::uint8_t b)
{
    return (b & 0xFE) == 0xE8 || (previous == 0x0F && (b & 0xF0) == 0x80);
}

class Bcj2Stream final : public io::InputStream {
public:
    Bcj2Stream(std::vector<io::StreamPtr> inputs, std::uint64_t size)
        : main_(std::move(inputs[0])), call_(std::move(inputs[1])), jump_(std::move(inputs[2])),
          range_coded_(std::move(inputs[3])), size_(size)
    {
        for (auto& p : probabilities_) {
            p = kBitModelTotal >> 1;
        }
        for (int i = 0; i < 5; ++i) {
            code_ = (code_ << 8) | range_coded_.read_byte();
        }
    }

    std::size_t read(std::uint8_t* data, std::size_t size) override
    {
        std::size_t n = 0;
        while (n < size && produced_ < size_) {
            if (pending_pos_ < pending_count_) {
                data[n++] = pending_[pending_pos_++];
                ++produced_;
                continue;
            }
            const std::uint8_t b = main_.read_byte();
            data[n++] = b;
            ++produced_;
            if (!is_jump(previous_, b)) {
                previous_ = b;
                continue;
            }
            if (produced_ == size_) {
                break;
            }
            std::uint16_t& probability = b == 0xE8 ? probabilities_[previous_] : probabilities_[b == 0xE9 ? 256 : 257];
            if (!decode_bit(probability)) {
                previous_ = b;
                continue;
            }
            InputBuffer& source = b == 0xE8 ? call_ : jump_;
            std::uint32_t target = 0;
            for (int i = 0; i < 4; ++i) {
                target = (target << 8) | source.read_byte();
            }
            const std::uint32_t relative = target - static_cast<std::uint32_t>(produced_ + 4);
            for (int i = 0; i < 4; ++i) {
                pending_[i] = static_cast<std::uint8_t>(relative >> (8 * i));
            }
            pending_count_ = 4;
            pending_pos_ = 0;
            previous_ = static_cast<std::uint8_t>(relative >> 24);
        }
        return n;
    }

private:
    bool decode_bit(std::uint16_t& probability)
    {
        const std::uint32_t bound = (range_ >> kBitModelBits) * probability;
        bool bit = false;
        if (code_ < bound) {
            range_ = bound;
            probability = static_cast<std::uint16_t>(probability + ((kBitModelTotal - probability) >> kMoveBits));
        } else {
            range_ -= bound;
            code_ -= bound;
            probability = static_cast<std::uint16_t>(probability - (probability >> kMoveBits));
            bit = true;
        }
        if (range_ < kTopValue) {
            range_ <<= 8;
            code_ = (code_ << 8) | range_coded_.read_byte();
        }
        return bit;
    }

    InputBuffer main_;
    InputBuffer call_;
    InputBuffer jump_;
    InputBuffer range_coded_;
    std::uint64_t size_;
    std::uint64_t produced_ = 0;

    std::uint16_t probabilities_[258];
    std::uint32_t range_ = 0xFFFFFFFF;
    std::uint32_t code_ = 0;
    std::uint8_t previous_ = 0;

    std::uint8_t pending_[4] = {};
    int pending_count_ = 0;
    int pending_pos_ = 0;
};

} // namespace

io::StreamPtr decode_bcj2(std::vector<io::StreamPtr> inputs, std::uint64_t size)
{
    if (inputs.size() != 4) {
        throw Error(ErrorCode::Corrupt, "bcj2: four input streams are required");
    }
    return std::make_unique<Bcj2Stream>(std::move(inputs), size);
}

} // namespace digzip::codec
