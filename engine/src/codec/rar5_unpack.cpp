// RAR 5 decompression, written for DigZip.
//
// The compressed data is a sequence of blocks. Each block starts on a byte boundary with a small
// header (flags, checksum, size) and may carry new Huffman tables. Symbols then encode literals,
// matches (length, distance) with a history of the last four distances, and filters that undo
// transformations applied before compression (x86 call addresses, ARM branches, byte deltas).

#include "codec/rar5.hpp"

#include <digzip/error.hpp>

#include <algorithm>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace digzip::codec {

namespace {

constexpr unsigned kNC = 306; // literals, filter, repeats, lengths
constexpr unsigned kDC = 64;  // distance slots
constexpr unsigned kLDC = 16; // low distance bits
constexpr unsigned kRC = 44;  // lengths of repeated matches
constexpr unsigned kBC = 20;  // bit lengths of the other tables
constexpr unsigned kTableSize = kNC + kDC + kLDC + kRC;

constexpr std::uint64_t kMinWindow = 8u << 20;      // holds the largest filter block with room to spare
constexpr std::uint64_t kDecodeMargin = 1u << 16;   // longest match is about 4 KiB
constexpr std::uint32_t kMaxFilterBlock = 0x400000;
constexpr std::size_t kMaxFilters = 8192;

enum FilterType : unsigned { kDelta = 0, kE8 = 1, kE8E9 = 2, kArm = 3 };

[[noreturn]] void corrupt(const char* what)
{
    throw Error(ErrorCode::Corrupt, std::string("rar: ") + what);
}

// MSB-first bit reader with absolute byte positions, zero-padded past the end of the data.
class BitInput {
public:
    void reset(io::StreamPtr stream)
    {
        stream_ = std::move(stream);
        buffer_.clear();
        pos_ = 0;
        bit_ = 0;
        base_ = 0;
        ended_ = false;
    }

    std::uint64_t byte_position() const { return base_ + pos_; }
    unsigned bit() const { return bit_; }

    // Keeps at least 16 bytes readable past the current position.
    void ensure()
    {
        if (buffer_.size() - pos_ >= 16) {
            return;
        }
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(pos_));
        base_ += pos_;
        pos_ = 0;
        while (!ended_ && buffer_.size() < 16) {
            const std::size_t old = buffer_.size();
            buffer_.resize(old + (1 << 16));
            const std::size_t n = stream_->read(buffer_.data() + old, buffer_.size() - old);
            buffer_.resize(old + n);
            if (n == 0) {
                ended_ = true;
                end_ = base_ + buffer_.size();
            }
        }
        if (ended_) {
            if (byte_position() > end_ + 8) {
                corrupt("unexpected end of compressed data");
            }
            buffer_.resize(std::max(buffer_.size(), pos_ + 16), 0);
        }
    }

    std::uint32_t get16() const
    {
        const std::uint8_t* p = buffer_.data() + pos_;
        const std::uint32_t v = (std::uint32_t{p[0]} << 16) | (std::uint32_t{p[1]} << 8) | p[2];
        return (v >> (8 - bit_)) & 0xFFFF;
    }

    std::uint32_t get32() const
    {
        const std::uint8_t* p = buffer_.data() + pos_;
        const std::uint64_t v = (std::uint64_t{p[0]} << 32) | (std::uint64_t{p[1]} << 24) | (std::uint64_t{p[2]} << 16) |
                                (std::uint64_t{p[3]} << 8) | p[4];
        return static_cast<std::uint32_t>(v >> (8 - bit_));
    }

    void skip(unsigned bits)
    {
        bits += bit_;
        pos_ += bits >> 3;
        bit_ = bits & 7;
    }

    void align()
    {
        if (bit_ != 0) {
            ++pos_;
            bit_ = 0;
        }
    }

private:
    io::StreamPtr stream_;
    std::vector<std::uint8_t> buffer_;
    std::size_t pos_ = 0;
    unsigned bit_ = 0;
    std::uint64_t base_ = 0;
    std::uint64_t end_ = 0;
    bool ended_ = false;
};

// Canonical Huffman decoder, with a lookup table for the short codes.
struct HuffmanTable {
    unsigned size = 0;
    std::uint32_t limit[16] = {};    // left-aligned 16-bit upper bound of the codes of each length
    std::uint32_t position[16] = {}; // index of the first symbol of each length
    unsigned quick_bits = 0;
    std::uint8_t quick_length[1 << 10] = {};
    std::uint16_t quick_symbol[1 << 10] = {};
    std::uint16_t symbols[kNC] = {};

    void build(const std::uint8_t* lengths, unsigned count)
    {
        size = count;
        unsigned length_count[16] = {};
        for (unsigned i = 0; i < count; ++i) {
            ++length_count[lengths[i] & 15];
        }
        length_count[0] = 0;
        std::fill(std::begin(symbols), std::end(symbols), std::uint16_t{0});
        position[0] = 0;
        limit[0] = 0;
        std::uint32_t upper = 0;
        for (unsigned i = 1; i < 16; ++i) {
            upper += length_count[i];
            limit[i] = upper << (16 - i);
            upper *= 2;
            position[i] = position[i - 1] + length_count[i - 1];
        }
        std::uint32_t next[16];
        std::copy(std::begin(position), std::end(position), next);
        for (unsigned i = 0; i < count; ++i) {
            const unsigned length = lengths[i] & 15;
            if (length != 0) {
                symbols[next[length]++] = static_cast<std::uint16_t>(i);
            }
        }

        quick_bits = count == kNC ? 10 : 7;
        unsigned length = 1;
        for (std::uint32_t code = 0; code < (1u << quick_bits); ++code) {
            const std::uint32_t bits = code << (16 - quick_bits);
            while (length < 16 && bits >= limit[length]) {
                ++length;
            }
            quick_length[code] = static_cast<std::uint8_t>(length);
            const std::uint32_t distance = (bits - limit[length - 1]) >> (16 - length);
            const std::uint32_t pos = length < 16 ? position[length] + distance : count;
            quick_symbol[code] = pos < count ? symbols[pos] : 0;
        }
    }

    unsigned decode(BitInput& in) const
    {
        const std::uint32_t bits = in.get16() & 0xFFFE;
        if (bits < limit[quick_bits]) {
            const std::uint32_t code = bits >> (16 - quick_bits);
            in.skip(quick_length[code]);
            return quick_symbol[code];
        }
        unsigned length = 15;
        for (unsigned i = quick_bits + 1; i < 15; ++i) {
            if (bits < limit[i]) {
                length = i;
                break;
            }
        }
        in.skip(length);
        std::uint32_t pos = position[length] + ((bits - limit[length - 1]) >> (16 - length));
        if (pos >= size) {
            pos = 0;
        }
        return symbols[pos];
    }
};

struct Filter {
    std::uint64_t start; // absolute output position
    std::uint32_t length;
    unsigned type;
    unsigned channels;
};

unsigned slot_to_length(BitInput& in, unsigned slot)
{
    unsigned bits = 0;
    unsigned length = 2;
    if (slot < 8) {
        length += slot;
    } else {
        bits = slot / 4 - 1;
        length += (4 | (slot & 3)) << bits;
    }
    if (bits > 0) {
        length += in.get16() >> (16 - bits);
        in.skip(bits);
    }
    return length;
}

} // namespace

struct Rar5Unpacker::State {
    BitInput in;
    std::vector<std::uint8_t> window;
    std::uint64_t mask = 0;
    std::uint64_t decoded = 0; // absolute position of the next decoded byte
    std::uint64_t written = 0; // absolute position of the next byte handed to the caller
    std::uint64_t file_start = 0;
    std::optional<std::uint64_t> file_size;
    bool file_done = false;

    bool in_block = false;
    std::uint64_t block_start = 0;
    std::uint32_t block_size = 0;
    unsigned block_bits = 0;
    bool last_block = false;
    bool tables_read = false;
    HuffmanTable main, distances, low_distances, repeats;

    std::uint64_t old_distance[4] = {};
    unsigned last_length = 0;
    std::deque<Filter> filters;
    std::vector<std::uint8_t> filtered;
    std::size_t filtered_pos = 0;

    void reset(std::uint64_t window_size)
    {
        if (window.size() != window_size) {
            window.assign(static_cast<std::size_t>(window_size), 0);
        }
        mask = window_size - 1;
        decoded = written = 0;
        tables_read = false;
        std::fill(std::begin(old_distance), std::end(old_distance), 0);
        last_length = 0;
        filters.clear();
    }

    bool block_finished() const
    {
        const std::uint64_t pos = in.byte_position();
        const std::uint64_t last = block_start + block_size - 1;
        return pos > last || (pos == last && in.bit() >= block_bits);
    }

    void read_block_header()
    {
        in.align();
        const unsigned flags = in.get16() >> 8;
        in.skip(8);
        const unsigned byte_count = ((flags >> 3) & 3) + 1;
        if (byte_count == 4) {
            corrupt("invalid block header");
        }
        const unsigned checksum = in.get16() >> 8;
        in.skip(8);
        std::uint32_t size = 0;
        for (unsigned i = 0; i < byte_count; ++i) {
            size += (in.get16() >> 8) << (i * 8);
            in.skip(8);
        }
        if (((0x5A ^ flags ^ size ^ (size >> 8) ^ (size >> 16)) & 0xFF) != checksum || size == 0) {
            corrupt("block header checksum mismatch");
        }
        block_start = in.byte_position();
        block_size = size;
        block_bits = (flags & 7) + 1;
        last_block = (flags & 0x40) != 0;
        in_block = true;
        if (flags & 0x80) {
            read_tables();
        } else if (!tables_read) {
            corrupt("block without Huffman tables");
        }
    }

    void read_tables()
    {
        std::uint8_t bit_lengths[kBC];
        for (unsigned i = 0; i < kBC; ++i) {
            const unsigned length = in.get16() >> 12;
            in.skip(4);
            if (length != 15) {
                bit_lengths[i] = static_cast<std::uint8_t>(length);
                continue;
            }
            unsigned zeros = in.get16() >> 12;
            in.skip(4);
            if (zeros == 0) {
                bit_lengths[i] = 15;
                continue;
            }
            for (zeros += 2; zeros > 0 && i < kBC; --zeros) {
                bit_lengths[i++] = 0;
            }
            --i;
        }
        HuffmanTable lengths_table;
        lengths_table.build(bit_lengths, kBC);

        std::uint8_t table[kTableSize];
        for (unsigned i = 0; i < kTableSize;) {
            in.ensure();
            const unsigned number = lengths_table.decode(in);
            if (number < 16) {
                table[i++] = static_cast<std::uint8_t>(number);
                continue;
            }
            unsigned count = 0;
            if (number == 16 || number == 18) {
                count = (in.get16() >> 13) + 3;
                in.skip(3);
            } else {
                count = (in.get16() >> 9) + 11;
                in.skip(7);
            }
            if (number < 18 && i == 0) {
                corrupt("repeat with no previous length");
            }
            for (; count > 0 && i < kTableSize; --count, ++i) {
                table[i] = number < 18 ? table[i - 1] : 0;
            }
        }
        main.build(table, kNC);
        distances.build(table + kNC, kDC);
        low_distances.build(table + kNC + kDC, kLDC);
        repeats.build(table + kNC + kDC + kLDC, kRC);
        tables_read = true;
    }

    std::uint32_t read_filter_number()
    {
        const unsigned byte_count = (in.get16() >> 14) + 1;
        in.skip(2);
        std::uint32_t value = 0;
        for (unsigned i = 0; i < byte_count; ++i) {
            value += (in.get16() >> 8) << (i * 8);
            in.skip(8);
        }
        return value;
    }

    void read_filter()
    {
        const std::uint32_t start = read_filter_number();
        const std::uint32_t length = read_filter_number();
        const unsigned type = in.get16() >> 13;
        in.skip(3);
        unsigned channels = 0;
        if (type == kDelta) {
            channels = (in.get16() >> 11) + 1;
            in.skip(5);
        }
        if (length == 0 || length > kMaxFilterBlock || type > kArm) {
            return; // ignored, as by RAR itself
        }
        const Filter filter{decoded + start, length, type, channels};
        if (filter.start < written ||
            (!filters.empty() && filter.start < filters.back().start + filters.back().length)) {
            corrupt("overlapping filters");
        }
        if (filters.size() == kMaxFilters) {
            corrupt("too many filters");
        }
        filters.push_back(filter);
    }

    void put(std::uint8_t byte)
    {
        window[static_cast<std::size_t>(decoded & mask)] = byte;
        ++decoded;
    }

    void copy(unsigned length, std::uint64_t distance)
    {
        if (distance == 0 || distance > decoded || distance > window.size()) {
            corrupt("match distance out of range");
        }
        for (unsigned i = 0; i < length; ++i) {
            put(window[static_cast<std::size_t>((decoded - distance) & mask)]);
        }
    }

    void remember(std::uint64_t distance)
    {
        old_distance[3] = old_distance[2];
        old_distance[2] = old_distance[1];
        old_distance[1] = old_distance[0];
        old_distance[0] = distance;
    }

    // Decodes symbols until the window is full of pending output, a chunk is done, or the file ends.
    void decode_some()
    {
        const std::uint64_t stop = written + window.size() - kDecodeMargin;
        const std::uint64_t chunk_end = decoded + (1u << 20);
        while (decoded < stop && decoded < chunk_end) {
            in.ensure();
            if (!in_block || block_finished()) {
                if (in_block && last_block) {
                    file_done = true;
                    return;
                }
                read_block_header();
                continue;
            }
            const unsigned symbol = main.decode(in);
            if (symbol < 256) {
                put(static_cast<std::uint8_t>(symbol));
            } else if (symbol >= 262) {
                unsigned length = slot_to_length(in, symbol - 262);
                const unsigned slot = distances.decode(in);
                std::uint64_t distance = 1;
                unsigned bits = 0;
                if (slot < 4) {
                    distance += slot;
                } else {
                    bits = slot / 2 - 1;
                    distance += std::uint64_t{2u | (slot & 1u)} << bits;
                }
                if (bits >= 4) {
                    if (bits > 4) {
                        distance += std::uint64_t{in.get32() >> (36 - bits)} << 4;
                        in.skip(bits - 4);
                    }
                    distance += low_distances.decode(in);
                } else if (bits > 0) {
                    distance += in.get32() >> (32 - bits);
                    in.skip(bits);
                }
                if (distance > 0x100) {
                    ++length;
                    if (distance > 0x2000) {
                        ++length;
                        if (distance > 0x40000) {
                            ++length;
                        }
                    }
                }
                remember(distance);
                last_length = length;
                copy(length, distance);
            } else if (symbol == 256) {
                read_filter();
            } else if (symbol == 257) {
                if (last_length != 0) {
                    copy(last_length, old_distance[0]);
                }
            } else {
                const unsigned index = symbol - 258;
                const std::uint64_t distance = old_distance[index];
                for (unsigned i = index; i > 0; --i) {
                    old_distance[i] = old_distance[i - 1];
                }
                old_distance[0] = distance;
                const unsigned length = slot_to_length(in, repeats.decode(in));
                last_length = length;
                copy(length, distance);
            }
        }
    }

    void apply_filter(const Filter& f)
    {
        std::vector<std::uint8_t> block(f.length);
        for (std::uint32_t i = 0; i < f.length; ++i) {
            block[i] = window[static_cast<std::size_t>((f.start + i) & mask)];
        }
        const auto file_offset = static_cast<std::uint32_t>(f.start - file_start);
        switch (f.type) {
        case kE8:
        case kE8E9: {
            constexpr std::uint32_t kFileSize = 0x1000000;
            const std::uint8_t second = f.type == kE8E9 ? 0xE9 : 0xE8;
            for (std::uint32_t pos = 0; pos + 4 < f.length;) {
                const std::uint8_t byte = block[pos++];
                if (byte != 0xE8 && byte != second) {
                    continue;
                }
                const std::uint32_t offset = (pos + file_offset) % kFileSize;
                std::uint8_t* p = block.data() + pos;
                const std::uint32_t address = std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) |
                                              (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[3]} << 24);
                std::uint32_t value = address;
                bool change = false;
                if (address & 0x80000000) {
                    if (((address + offset) & 0x80000000) == 0) {
                        value = address + kFileSize;
                        change = true;
                    }
                } else if (((address - kFileSize) & 0x80000000) != 0) {
                    value = address - offset;
                    change = true;
                }
                if (change) {
                    for (int k = 0; k < 4; ++k) {
                        p[k] = static_cast<std::uint8_t>(value >> (8 * k));
                    }
                }
                pos += 4;
            }
            filtered = std::move(block);
            break;
        }
        case kArm:
            for (std::uint32_t pos = 0; pos + 3 < f.length; pos += 4) {
                std::uint8_t* d = block.data() + pos;
                if (d[3] == 0xEB) {
                    std::uint32_t offset = d[0] + (std::uint32_t{d[1]} << 8) + (std::uint32_t{d[2]} << 16);
                    offset -= (file_offset + pos) / 4;
                    d[0] = static_cast<std::uint8_t>(offset);
                    d[1] = static_cast<std::uint8_t>(offset >> 8);
                    d[2] = static_cast<std::uint8_t>(offset >> 16);
                }
            }
            filtered = std::move(block);
            break;
        default: { // delta: the bytes of each channel are stored one after the other
            filtered.assign(f.length, 0);
            std::size_t source = 0;
            for (unsigned channel = 0; channel < f.channels; ++channel) {
                std::uint8_t previous = 0;
                for (std::size_t dest = channel; dest < f.length; dest += f.channels) {
                    previous = static_cast<std::uint8_t>(previous - block[source++]);
                    filtered[dest] = previous;
                }
            }
            break;
        }
        }
        filtered_pos = 0;
    }
};

Rar5Unpacker::Rar5Unpacker() : state_(std::make_unique<State>()) {}

Rar5Unpacker::~Rar5Unpacker() = default;

void Rar5Unpacker::start(io::StreamPtr packed, std::optional<std::uint64_t> size, std::uint64_t dictionary, bool solid)
{
    State& s = *state_;
    std::uint64_t window_size = kMinWindow;
    while (window_size < dictionary) {
        window_size <<= 1;
    }
    if (!solid || s.window.empty()) {
        s.reset(window_size);
    } else if (window_size > s.window.size() || s.decoded != s.written) {
        corrupt("solid file does not continue the previous one");
    }
    s.in.reset(std::move(packed));
    s.in_block = false;
    s.file_done = false;
    s.file_start = s.written;
    s.file_size = size;
    s.filters.clear();
    s.filtered.clear();
    s.filtered_pos = 0;
}

std::size_t Rar5Unpacker::read(std::uint8_t* data, std::size_t size)
{
    State& s = *state_;
    std::size_t produced = 0;
    while (produced < size) {
        const std::uint64_t file_end = s.file_size ? s.file_start + *s.file_size : UINT64_MAX;
        if (s.written >= file_end) {
            break;
        }
        if (s.filtered_pos < s.filtered.size()) {
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(
                {s.filtered.size() - s.filtered_pos, size - produced, file_end - s.written}));
            std::memcpy(data + produced, s.filtered.data() + s.filtered_pos, n);
            s.filtered_pos += n;
            s.written += n;
            produced += n;
            continue;
        }
        std::uint64_t limit = std::min(s.decoded, file_end);
        if (!s.filters.empty()) {
            limit = std::min(limit, s.filters.front().start);
        }
        if (s.written < limit) {
            const std::size_t n =
                static_cast<std::size_t>(std::min<std::uint64_t>(limit - s.written, size - produced));
            for (std::size_t i = 0; i < n; ++i) {
                data[produced + i] = s.window[static_cast<std::size_t>((s.written + i) & s.mask)];
            }
            s.written += n;
            produced += n;
            continue;
        }
        if (!s.filters.empty() && s.written == s.filters.front().start &&
            s.decoded >= s.filters.front().start + s.filters.front().length) {
            s.apply_filter(s.filters.front());
            s.filters.pop_front();
            continue;
        }
        if (s.file_done) {
            if (s.file_size && s.written < file_end) {
                corrupt("the file holds less data than declared");
            }
            break;
        }
        s.decode_some();
    }
    return produced;
}

} // namespace digzip::codec
