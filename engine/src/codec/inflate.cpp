// Inflater written for DigZip, used for Deflate64 (ZIP method 9), which zlib does not decode.
//
// Deflate64 is deflate (RFC 1951) with three changes: a 64 KiB window, length code 285 meaning
// "3 + 16 extra bits" instead of a fixed 258, and distance codes 30 and 31 (14 extra bits each).
// Huffman codes are decoded canonically, one bit at a time: simple to verify, and fast enough for
// a format that is rare in practice.

#include "codec/decoders.hpp"
#include "codec/input_buffer.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace digzip::codec {

namespace {

constexpr std::size_t kMaxBits = 15;
constexpr std::size_t kWindowSize = 1 << 16;
constexpr std::size_t kWindowMask = kWindowSize - 1;

constexpr std::uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                           31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                           2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::uint32_t kDistanceBase[32] = {1,    2,    3,    4,    5,    7,     9,     13,    17,    25,   33,
                                             49,   65,   97,   129,  193,  257,   385,   513,   769,   1025, 1537,
                                             2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577, 32769, 49153};
constexpr std::uint8_t kDistanceExtra[32] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,  6,
                                             7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14};
constexpr std::uint8_t kCodeLengthOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

[[noreturn]] void corrupt(const char* what)
{
    throw Error(ErrorCode::Corrupt, std::string("deflate: ") + what);
}

// Canonical Huffman code: how many codes of each length, and the symbols sorted by code.
struct Huffman {
    std::array<std::uint16_t, kMaxBits + 1> count{};
    std::array<std::uint16_t, 320> symbol{};

    void build(const std::uint8_t* lengths, int n)
    {
        count.fill(0);
        for (int i = 0; i < n; ++i) {
            ++count[lengths[i]];
        }
        int left = 1;
        for (std::size_t len = 1; len <= kMaxBits; ++len) {
            left = (left << 1) - count[len];
            if (left < 0) {
                corrupt("over-subscribed Huffman code");
            }
        }
        std::array<std::uint16_t, kMaxBits + 1> offset{};
        for (std::size_t len = 1; len < kMaxBits; ++len) {
            offset[len + 1] = static_cast<std::uint16_t>(offset[len] + count[len]);
        }
        for (int i = 0; i < n; ++i) {
            if (lengths[i] != 0) {
                symbol[offset[lengths[i]]++] = static_cast<std::uint16_t>(i);
            }
        }
    }
};

class Inflater final : public io::InputStream {
public:
    Inflater(io::StreamPtr upstream, bool deflate64)
        : in_(std::move(upstream)), deflate64_(deflate64), window_(kWindowSize)
    {
    }

    std::size_t read(std::uint8_t* data, std::size_t size) override
    {
        out_ = data;
        produced_ = 0;
        while (produced_ < size) {
            if (copy_length_ > 0) {
                copy_match(size);
                continue;
            }
            switch (state_) {
            case State::BlockHeader:
                if (last_block_) {
                    state_ = State::Done;
                } else {
                    start_block();
                }
                break;
            case State::Stored:
                copy_stored(size);
                break;
            case State::Huffman:
                decode_symbol();
                break;
            case State::Done:
                return produced_;
            }
        }
        return produced_;
    }

private:
    enum class State { BlockHeader, Stored, Huffman, Done };

    std::uint32_t bits(int count)
    {
        while (bit_count_ < count) {
            bit_buffer_ |= static_cast<std::uint64_t>(in_.read_byte()) << bit_count_;
            bit_count_ += 8;
        }
        const auto value = static_cast<std::uint32_t>(bit_buffer_ & ((std::uint64_t{1} << count) - 1));
        bit_buffer_ >>= count;
        bit_count_ -= count;
        return value;
    }

    int decode(const Huffman& h)
    {
        int code = 0;
        int first = 0;
        int index = 0;
        for (std::size_t len = 1; len <= kMaxBits; ++len) {
            code |= static_cast<int>(bits(1));
            const int count = h.count[len];
            if (code - count < first) {
                return h.symbol[static_cast<std::size_t>(index + (code - first))];
            }
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        corrupt("invalid Huffman code");
    }

    void emit(std::uint8_t byte)
    {
        out_[produced_++] = byte;
        window_[window_pos_++ & kWindowMask] = byte;
        ++total_out_;
    }

    void start_block()
    {
        last_block_ = bits(1) == 1;
        switch (bits(2)) {
        case 0: {
            const int skip = bit_count_ % 8;
            bits(skip);
            const std::uint32_t length = bits(16);
            if ((length ^ 0xFFFF) != bits(16)) {
                corrupt("stored block length check failed");
            }
            stored_left_ = length;
            state_ = State::Stored;
            break;
        }
        case 1:
            build_fixed_codes();
            state_ = State::Huffman;
            break;
        case 2:
            build_dynamic_codes();
            state_ = State::Huffman;
            break;
        default:
            corrupt("invalid block type");
        }
    }

    void build_fixed_codes()
    {
        std::uint8_t lengths[288 + 32];
        std::memset(lengths, 8, 144);
        std::memset(lengths + 144, 9, 112);
        std::memset(lengths + 256, 7, 24);
        std::memset(lengths + 280, 8, 8);
        literals_.build(lengths, 288);
        std::memset(lengths, 5, 32);
        distances_.build(lengths, 32);
    }

    void build_dynamic_codes()
    {
        const int literal_count = static_cast<int>(bits(5)) + 257;
        const int distance_count = static_cast<int>(bits(5)) + 1;
        const int code_length_count = static_cast<int>(bits(4)) + 4;
        if (literal_count > 286 || (!deflate64_ && distance_count > 30)) {
            corrupt("too many length or distance codes");
        }

        std::uint8_t lengths[288 + 32] = {};
        for (int i = 0; i < code_length_count; ++i) {
            lengths[kCodeLengthOrder[i]] = static_cast<std::uint8_t>(bits(3));
        }
        Huffman code_lengths;
        code_lengths.build(lengths, 19);

        const int total = literal_count + distance_count;
        int index = 0;
        while (index < total) {
            const int symbol = decode(code_lengths);
            if (symbol < 16) {
                lengths[index++] = static_cast<std::uint8_t>(symbol);
                continue;
            }
            std::uint8_t value = 0;
            int repeat = 0;
            if (symbol == 16) {
                if (index == 0) {
                    corrupt("repeat with no previous length");
                }
                value = lengths[index - 1];
                repeat = 3 + static_cast<int>(bits(2));
            } else if (symbol == 17) {
                repeat = 3 + static_cast<int>(bits(3));
            } else {
                repeat = 11 + static_cast<int>(bits(7));
            }
            if (index + repeat > total) {
                corrupt("too many code lengths");
            }
            std::memset(lengths + index, value, static_cast<std::size_t>(repeat));
            index += repeat;
        }
        if (lengths[256] == 0) {
            corrupt("missing end-of-block code");
        }
        literals_.build(lengths, literal_count);
        distances_.build(lengths + literal_count, distance_count);
    }

    void decode_symbol()
    {
        const int symbol = decode(literals_);
        if (symbol < 256) {
            emit(static_cast<std::uint8_t>(symbol));
            return;
        }
        if (symbol == 256) {
            state_ = State::BlockHeader;
            return;
        }
        const int length_code = symbol - 257;
        if (length_code >= 29) {
            corrupt("invalid length code");
        }
        std::uint32_t length = 0;
        if (deflate64_ && length_code == 28) {
            length = 3 + bits(16);
        } else {
            length = kLengthBase[length_code] + bits(kLengthExtra[length_code]);
        }
        const int distance_code = decode(distances_);
        if (distance_code >= (deflate64_ ? 32 : 30)) {
            corrupt("invalid distance code");
        }
        const std::uint32_t distance = kDistanceBase[distance_code] + bits(kDistanceExtra[distance_code]);
        if (distance > total_out_) {
            corrupt("distance points before the start of the data");
        }
        copy_length_ = length;
        copy_distance_ = distance;
    }

    void copy_match(std::size_t size)
    {
        while (copy_length_ > 0 && produced_ < size) {
            emit(window_[(window_pos_ - copy_distance_) & kWindowMask]);
            --copy_length_;
        }
    }

    void copy_stored(std::size_t size)
    {
        if (stored_left_ == 0) {
            state_ = State::BlockHeader;
            return;
        }
        // Whole bytes still in the bit buffer come first, then the input buffer directly.
        while (stored_left_ > 0 && produced_ < size && bit_count_ >= 8) {
            emit(static_cast<std::uint8_t>(bits(8)));
            --stored_left_;
        }
        while (stored_left_ > 0 && produced_ < size) {
            if (!in_.fill()) {
                corrupt("unexpected end of compressed data");
            }
            std::size_t n = std::min<std::size_t>({stored_left_, size - produced_, in_.available()});
            for (const std::uint8_t* p = in_.data(); n > 0; --n, ++p) {
                emit(*p);
                in_.consume(1);
                --stored_left_;
            }
        }
    }

    InputBuffer in_;
    bool deflate64_;
    std::vector<std::uint8_t> window_;
    std::size_t window_pos_ = 0;
    std::uint64_t total_out_ = 0;

    std::uint64_t bit_buffer_ = 0;
    int bit_count_ = 0;

    State state_ = State::BlockHeader;
    bool last_block_ = false;
    std::uint32_t stored_left_ = 0;
    std::uint32_t copy_length_ = 0;
    std::uint32_t copy_distance_ = 0;
    Huffman literals_;
    Huffman distances_;

    std::uint8_t* out_ = nullptr;
    std::size_t produced_ = 0;
};

} // namespace

io::StreamPtr decode_inflate(io::StreamPtr upstream, bool deflate64)
{
    return std::make_unique<Inflater>(std::move(upstream), deflate64);
}

} // namespace digzip::codec
