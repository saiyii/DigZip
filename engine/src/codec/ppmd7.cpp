// PPMd variant H decoder (7z method 030401), written for DigZip from Dmitry Shkarin's public
// PPMd algorithm. The decoder rebuilds, symbol by symbol, the exact model the encoder had, so every
// detail matters: context statistics, escape estimation (SEE), the sub-allocator and the moments
// it runs out of memory and restarts the model.
//
// The model lives in one byte array addressed by 32-bit offsets, like the reference
// implementation. Offset 0 is never a valid node, so it stands for "null".

#include "codec/decoders.hpp"
#include "codec/input_buffer.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>

namespace digzip::codec {

namespace {

constexpr unsigned kMaxOrder = 64;
constexpr unsigned kNumIndexes = 38;
constexpr std::uint32_t kUnitSize = 12;
constexpr unsigned kMaxFreq = 124;
constexpr unsigned kIntBits = 7;
constexpr unsigned kPeriodBits = 7;
constexpr std::uint32_t kBinScale = 1u << (kIntBits + kPeriodBits);
constexpr std::uint32_t kTopValue = 1u << 24;
constexpr std::uint32_t kStateSize = 6;

constexpr std::uint16_t kInitBinEsc[8] = {0x3CDD, 0x1F3F, 0x59BF, 0x48F3, 0x64A1, 0x5ABC, 0x6632, 0x6051};
constexpr std::uint8_t kExpEscape[16] = {25, 14, 9, 7, 5, 5, 4, 4, 4, 3, 3, 3, 2, 2, 2, 2};

unsigned get_mean(unsigned prob)
{
    return (prob + (1u << (kPeriodBits - 2))) >> kPeriodBits;
}

// Secondary escape estimation.
struct See {
    std::uint16_t summ = 0;
    std::uint8_t shift = 0;
    std::uint8_t count = 0;

    void update()
    {
        if (shift < kPeriodBits && --count == 0) {
            summ = static_cast<std::uint16_t>(summ << 1);
            count = static_cast<std::uint8_t>(3 << shift++);
        }
    }
};

// Range decoder of the 7z flavour of PPMd.
class RangeDecoder {
public:
    explicit RangeDecoder(io::StreamPtr upstream) : in_(std::move(upstream)) {}

    void init()
    {
        if (byte() != 0) {
            throw Error(ErrorCode::Corrupt, "ppmd: invalid stream start");
        }
        for (int i = 0; i < 4; ++i) {
            code_ = (code_ << 8) | byte();
        }
        if (code_ == 0xFFFFFFFF) {
            throw Error(ErrorCode::Corrupt, "ppmd: invalid stream start");
        }
    }

    std::uint32_t threshold(std::uint32_t total) { return code_ / (range_ /= total); }

    void decode(std::uint32_t start, std::uint32_t size)
    {
        code_ -= start * range_;
        range_ *= size;
        normalize();
    }

    std::uint32_t decode_bit(std::uint32_t size0, std::uint32_t total)
    {
        const std::uint32_t bound = (range_ / total) * size0;
        std::uint32_t bit = 0;
        if (code_ < bound) {
            range_ = bound;
        } else {
            code_ -= bound;
            range_ -= bound;
            bit = 1;
        }
        normalize();
        return bit;
    }

private:
    // Past the end of the input the decoder reads zeros, like the reference; a stream that
    // keeps reading far past its end is truncated.
    std::uint8_t byte()
    {
        if (in_.fill()) {
            const std::uint8_t b = *in_.data();
            in_.consume(1);
            return b;
        }
        if (++overrun_ > 16) {
            throw Error(ErrorCode::Corrupt, "ppmd: unexpected end of compressed data");
        }
        return 0;
    }

    void normalize()
    {
        while (range_ < kTopValue) {
            code_ = (code_ << 8) | byte();
            range_ <<= 8;
        }
    }

    InputBuffer in_;
    std::uint32_t range_ = 0xFFFFFFFF;
    std::uint32_t code_ = 0;
    unsigned overrun_ = 0;
};

class Model {
public:
    Model(std::uint32_t size, unsigned max_order) : size_(size), max_order_(max_order)
    {
        align_offset_ = 4 - (size & 3);
        // One extra unit at the end holds the list head used while gluing free blocks.
        memory_.reset(new std::uint8_t[std::size_t{align_offset_} + size + kUnitSize]);

        for (unsigned i = 0, k = 0; i < kNumIndexes; ++i) {
            unsigned step = i >= 12 ? 4 : (i >> 2) + 1;
            do {
                units_to_index_[k++] = static_cast<std::uint8_t>(i);
            } while (--step);
            index_to_units_[i] = static_cast<std::uint8_t>(k);
        }

        ns_to_bs_index_[0] = 0 << 1;
        ns_to_bs_index_[1] = 1 << 1;
        std::fill(ns_to_bs_index_.begin() + 2, ns_to_bs_index_.begin() + 11, std::uint8_t{2 << 1});
        std::fill(ns_to_bs_index_.begin() + 11, ns_to_bs_index_.end(), std::uint8_t{3 << 1});

        unsigned i = 0;
        for (; i < 3; ++i) {
            ns_to_index_[i] = static_cast<std::uint8_t>(i);
        }
        for (unsigned m = i, k = 1; i < 256; ++i) {
            ns_to_index_[i] = static_cast<std::uint8_t>(m);
            if (--k == 0) {
                k = ++m - 2;
            }
        }

        std::fill(hb_to_flag_.begin(), hb_to_flag_.begin() + 0x40, std::uint8_t{0});
        std::fill(hb_to_flag_.begin() + 0x40, hb_to_flag_.end(), std::uint8_t{8});

        restart();
        dummy_see_.shift = kPeriodBits;
        dummy_see_.summ = 0;
        dummy_see_.count = 64;
    }

    // Returns the symbol, -1 at the end marker, -2 on invalid data.
    int decode_symbol(RangeDecoder& rc)
    {
        std::array<std::int8_t, 256> mask;
        if (num_stats(min_context_) != 1) {
            std::uint32_t s = stats(min_context_);
            std::uint32_t count = rc.threshold(summ_freq(min_context_));
            std::uint32_t high = freq(s);
            if (count < high) {
                rc.decode(0, high);
                found_state_ = s;
                const int symbol = sym(s);
                update1_0();
                return symbol;
            }
            prev_success_ = 0;
            unsigned i = num_stats(min_context_) - 1u;
            do {
                s += kStateSize;
                if ((high += freq(s)) > count) {
                    rc.decode(high - freq(s), freq(s));
                    found_state_ = s;
                    const int symbol = sym(s);
                    update1();
                    return symbol;
                }
            } while (--i);
            if (count >= summ_freq(min_context_)) {
                return -2;
            }
            hi_bits_flag_ = hb_to_flag_[sym(found_state_)];
            rc.decode(high, summ_freq(min_context_) - high);
            mask.fill(-1);
            mask[sym(s)] = 0;
            i = num_stats(min_context_) - 1u;
            do {
                s -= kStateSize;
                mask[sym(s)] = 0;
            } while (--i);
        } else {
            std::uint16_t& prob = bin_summ();
            if (rc.decode_bit(prob, kBinScale) == 0) {
                prob = static_cast<std::uint16_t>(prob + (1u << kIntBits) - get_mean(prob));
                found_state_ = one_state(min_context_);
                const int symbol = sym(found_state_);
                update_bin();
                return symbol;
            }
            prob = static_cast<std::uint16_t>(prob - get_mean(prob));
            init_esc_ = kExpEscape[prob >> 10];
            mask.fill(-1);
            mask[sym(one_state(min_context_))] = 0;
            prev_success_ = 0;
        }

        for (;;) {
            std::uint32_t states[256];
            const unsigned masked = num_stats(min_context_);
            do {
                ++order_fall_;
                if (suffix(min_context_) == 0) {
                    return -1;
                }
                min_context_ = suffix(min_context_);
            } while (num_stats(min_context_) == masked);

            std::uint32_t high = 0;
            std::uint32_t s = stats(min_context_);
            const unsigned visible = num_stats(min_context_) - masked;
            unsigned i = 0;
            do {
                const int k = mask[sym(s)];
                high += freq(s) & static_cast<std::uint32_t>(k);
                states[i] = s;
                s += kStateSize;
                i -= static_cast<unsigned>(k);
            } while (i != visible);

            std::uint32_t freq_sum = 0;
            See& see = make_esc_freq(masked, freq_sum);
            freq_sum += high;
            const std::uint32_t count = rc.threshold(freq_sum);

            if (count < high) {
                std::uint32_t* p = states;
                for (high = 0; (high += freq(*p)) <= count; ++p) {
                }
                s = *p;
                rc.decode(high - freq(s), freq(s));
                see.update();
                found_state_ = s;
                const int symbol = sym(s);
                update2();
                return symbol;
            }
            if (count >= freq_sum) {
                return -2;
            }
            rc.decode(high, freq_sum - high);
            see.summ = static_cast<std::uint16_t>(see.summ + freq_sum);
            do {
                mask[sym(states[--i])] = 0;
            } while (i != 0);
        }
    }

private:
    // --- raw memory access ----------------------------------------------------------------

    std::uint16_t rd16(std::uint32_t at) const
    {
        std::uint16_t v;
        std::memcpy(&v, &memory_[at], 2);
        return v;
    }
    void wr16(std::uint32_t at, unsigned v)
    {
        const auto value = static_cast<std::uint16_t>(v);
        std::memcpy(&memory_[at], &value, 2);
    }
    std::uint32_t rd32(std::uint32_t at) const
    {
        std::uint32_t v;
        std::memcpy(&v, &memory_[at], 4);
        return v;
    }
    void wr32(std::uint32_t at, std::uint32_t v) { std::memcpy(&memory_[at], &v, 4); }

    // State: Symbol, Freq, Successor (32 bits, unaligned). 6 bytes.
    std::uint8_t sym(std::uint32_t s) const { return memory_[s]; }
    std::uint8_t& freq(std::uint32_t s) { return memory_[s + 1]; }
    std::uint32_t successor(std::uint32_t s) const { return rd32(s + 2); }
    void set_successor(std::uint32_t s, std::uint32_t v) { wr32(s + 2, v); }
    void copy_state(std::uint32_t to, std::uint32_t from) { std::memmove(&memory_[to], &memory_[from], kStateSize); }

    // Context: NumStats (16), SummFreq (16), Stats (32), Suffix (32). 12 bytes.
    // With a single state, the state itself is stored in place of SummFreq and Stats.
    unsigned num_stats(std::uint32_t c) const { return rd16(c); }
    void set_num_stats(std::uint32_t c, unsigned v) { wr16(c, v); }
    unsigned summ_freq(std::uint32_t c) const { return rd16(c + 2); }
    void set_summ_freq(std::uint32_t c, unsigned v) { wr16(c + 2, v); }
    std::uint32_t stats(std::uint32_t c) const { return rd32(c + 4); }
    void set_stats(std::uint32_t c, std::uint32_t v) { wr32(c + 4, v); }
    std::uint32_t suffix(std::uint32_t c) const { return rd32(c + 8); }
    void set_suffix(std::uint32_t c, std::uint32_t v) { wr32(c + 8, v); }
    static std::uint32_t one_state(std::uint32_t c) { return c + 2; }

    // Free block while gluing: Stamp (16), NU (16), Next (32), Prev (32).
    // In a free list, the first 32 bits hold the link to the next free block.
    unsigned node_stamp(std::uint32_t n) const { return rd16(n); }
    unsigned node_nu(std::uint32_t n) const { return rd16(n + 2); }
    std::uint32_t node_next(std::uint32_t n) const { return rd32(n + 4); }
    std::uint32_t node_prev(std::uint32_t n) const { return rd32(n + 8); }

    // --- sub-allocator --------------------------------------------------------------------

    unsigned i2u(unsigned index) const { return index_to_units_[index]; }
    unsigned u2i(unsigned units) const { return units_to_index_[units - 1]; }
    static std::uint32_t u2b(unsigned units) { return units * kUnitSize; }

    void insert_node(std::uint32_t node, unsigned index)
    {
        wr32(node, free_list_[index]);
        free_list_[index] = node;
    }

    std::uint32_t remove_node(unsigned index)
    {
        const std::uint32_t node = free_list_[index];
        free_list_[index] = rd32(node);
        return node;
    }

    void split_block(std::uint32_t ptr, unsigned old_index, unsigned new_index)
    {
        const unsigned nu = i2u(old_index) - i2u(new_index);
        ptr += u2b(i2u(new_index));
        unsigned i = u2i(nu);
        if (i2u(i) != nu) {
            const unsigned k = i2u(--i);
            insert_node(ptr + u2b(k), nu - k - 1);
        }
        insert_node(ptr, i);
    }

    void glue_free_blocks()
    {
        const std::uint32_t head = align_offset_ + size_;
        std::uint32_t n = head;
        glue_count_ = 255;

        // Doubly-linked list of every free block.
        for (unsigned i = 0; i < kNumIndexes; ++i) {
            const unsigned nu = i2u(i);
            std::uint32_t next = free_list_[i];
            free_list_[i] = 0;
            while (next != 0) {
                const std::uint32_t node = next;
                wr32(node + 4, n);
                wr32(n + 8, next);
                n = next;
                next = rd32(node);
                wr16(node, 0);
                wr16(node + 2, nu);
            }
        }
        wr16(head, 1);
        wr32(head + 4, n);
        wr32(n + 8, head);
        if (lo_unit_ != hi_unit_) {
            wr16(lo_unit_, 1);
        }

        // Merge physically adjacent free blocks.
        while (n != head) {
            const std::uint32_t node = n;
            std::uint32_t nu = node_nu(node);
            for (;;) {
                const std::uint32_t node2 = node + nu * kUnitSize;
                nu += node_nu(node2);
                if (node_stamp(node2) != 0 || nu >= 0x10000) {
                    break;
                }
                wr32(node_prev(node2) + 4, node_next(node2));
                wr32(node_next(node2) + 8, node_prev(node2));
                wr16(node + 2, nu);
            }
            n = node_next(node);
        }

        // Refill the free lists.
        for (n = node_next(head); n != head;) {
            std::uint32_t node = n;
            const std::uint32_t next = node_next(node);
            unsigned nu = node_nu(node);
            for (; nu > 128; nu -= 128, node += 128 * kUnitSize) {
                insert_node(node, kNumIndexes - 1);
            }
            unsigned i = u2i(nu);
            if (i2u(i) != nu) {
                const unsigned k = i2u(--i);
                insert_node(node + k * kUnitSize, nu - k - 1);
            }
            insert_node(node, i);
            n = next;
        }
    }

    std::uint32_t alloc_units_rare(unsigned index)
    {
        if (glue_count_ == 0) {
            glue_free_blocks();
            if (free_list_[index] != 0) {
                return remove_node(index);
            }
        }
        unsigned i = index;
        do {
            if (++i == kNumIndexes) {
                const std::uint32_t bytes = u2b(i2u(index));
                --glue_count_;
                return units_start_ - text_ > bytes ? (units_start_ -= bytes) : 0;
            }
        } while (free_list_[i] == 0);
        const std::uint32_t block = remove_node(i);
        split_block(block, i, index);
        return block;
    }

    std::uint32_t alloc_units(unsigned index)
    {
        if (free_list_[index] != 0) {
            return remove_node(index);
        }
        const std::uint32_t bytes = u2b(i2u(index));
        if (bytes <= hi_unit_ - lo_unit_) {
            const std::uint32_t block = lo_unit_;
            lo_unit_ += bytes;
            return block;
        }
        return alloc_units_rare(index);
    }

    std::uint32_t shrink_units(std::uint32_t old_ptr, unsigned old_nu, unsigned new_nu)
    {
        const unsigned i0 = u2i(old_nu);
        const unsigned i1 = u2i(new_nu);
        if (i0 == i1) {
            return old_ptr;
        }
        if (free_list_[i1] != 0) {
            const std::uint32_t ptr = remove_node(i1);
            std::memmove(&memory_[ptr], &memory_[old_ptr], u2b(new_nu));
            insert_node(old_ptr, i0);
            return ptr;
        }
        split_block(old_ptr, i0, i1);
        return old_ptr;
    }

    // --- model ----------------------------------------------------------------------------

    void restart()
    {
        std::fill(std::begin(free_list_), std::end(free_list_), 0u);
        text_ = align_offset_;
        hi_unit_ = text_ + size_;
        lo_unit_ = units_start_ = hi_unit_ - size_ / 8 / kUnitSize * 7 * kUnitSize;
        glue_count_ = 0;

        order_fall_ = max_order_;
        run_length_ = init_rl_ = -static_cast<std::int32_t>(max_order_ < 12 ? max_order_ : 12) - 1;
        prev_success_ = 0;

        hi_unit_ -= kUnitSize;
        min_context_ = max_context_ = hi_unit_;
        set_suffix(min_context_, 0);
        set_num_stats(min_context_, 256);
        set_summ_freq(min_context_, 256 + 1);
        found_state_ = lo_unit_;
        lo_unit_ += u2b(256 / 2);
        set_stats(min_context_, found_state_);
        for (unsigned i = 0; i < 256; ++i) {
            const std::uint32_t s = found_state_ + i * kStateSize;
            memory_[s] = static_cast<std::uint8_t>(i);
            freq(s) = 1;
            set_successor(s, 0);
        }

        for (unsigned i = 0; i < 128; ++i) {
            for (unsigned k = 0; k < 8; ++k) {
                const auto value = static_cast<std::uint16_t>(kBinScale - kInitBinEsc[k] / (i + 2));
                for (unsigned m = 0; m < 64; m += 8) {
                    bin_summ_[i][k + m] = value;
                }
            }
        }
        for (unsigned i = 0; i < 25; ++i) {
            for (See& s : see_[i]) {
                s.shift = kPeriodBits - 4;
                s.summ = static_cast<std::uint16_t>((5 * i + 10) << s.shift);
                s.count = 4;
            }
        }
    }

    std::uint32_t create_successors(bool skip)
    {
        std::uint32_t c = min_context_;
        const std::uint32_t up_branch = successor(found_state_);
        std::uint32_t states[kMaxOrder];
        unsigned count = 0;
        if (!skip) {
            states[count++] = found_state_;
        }
        while (suffix(c) != 0) {
            c = suffix(c);
            std::uint32_t s;
            if (num_stats(c) != 1) {
                for (s = stats(c); sym(s) != sym(found_state_); s += kStateSize) {
                }
            } else {
                s = one_state(c);
            }
            const std::uint32_t next = successor(s);
            if (next != up_branch) {
                c = next;
                if (count == 0) {
                    return c;
                }
                break;
            }
            states[count++] = s;
        }

        const std::uint8_t up_symbol = memory_[up_branch];
        unsigned up_freq;
        if (num_stats(c) == 1) {
            up_freq = freq(one_state(c));
        } else {
            std::uint32_t s;
            for (s = stats(c); sym(s) != up_symbol; s += kStateSize) {
            }
            const unsigned cf = freq(s) - 1u;
            const unsigned s0 = summ_freq(c) - num_stats(c) - cf;
            up_freq = 1 + ((2 * cf <= s0) ? (5 * cf > s0) : ((2 * cf + 3 * s0 - 1) / (2 * s0)));
        }

        do {
            std::uint32_t c1;
            if (hi_unit_ != lo_unit_) {
                c1 = (hi_unit_ -= kUnitSize);
            } else if (free_list_[0] != 0) {
                c1 = remove_node(0);
            } else {
                c1 = alloc_units_rare(0);
                if (c1 == 0) {
                    return 0;
                }
            }
            set_num_stats(c1, 1);
            const std::uint32_t one = one_state(c1);
            memory_[one] = up_symbol;
            freq(one) = static_cast<std::uint8_t>(up_freq);
            set_successor(one, up_branch + 1);
            set_suffix(c1, c);
            set_successor(states[--count], c1);
            c = c1;
        } while (count != 0);
        return c;
    }

    void swap_states(std::uint32_t a, std::uint32_t b)
    {
        std::uint8_t tmp[kStateSize];
        std::memcpy(tmp, &memory_[a], kStateSize);
        std::memcpy(&memory_[a], &memory_[b], kStateSize);
        std::memcpy(&memory_[b], tmp, kStateSize);
    }

    void update_model()
    {
        std::uint32_t f_successor = successor(found_state_);
        const std::uint8_t found_symbol = sym(found_state_);
        const unsigned found_freq = freq(found_state_);

        if (found_freq < kMaxFreq / 4 && suffix(min_context_) != 0) {
            const std::uint32_t c = suffix(min_context_);
            if (num_stats(c) == 1) {
                const std::uint32_t s = one_state(c);
                if (freq(s) < 32) {
                    ++freq(s);
                }
            } else {
                std::uint32_t s = stats(c);
                if (sym(s) != found_symbol) {
                    do {
                        s += kStateSize;
                    } while (sym(s) != found_symbol);
                    if (freq(s) >= freq(s - kStateSize)) {
                        swap_states(s, s - kStateSize);
                        s -= kStateSize;
                    }
                }
                if (freq(s) < kMaxFreq - 9) {
                    freq(s) = static_cast<std::uint8_t>(freq(s) + 2);
                    set_summ_freq(c, summ_freq(c) + 2);
                }
            }
        }

        if (order_fall_ == 0) {
            min_context_ = max_context_ = create_successors(true);
            if (min_context_ == 0) {
                restart();
                return;
            }
            set_successor(found_state_, min_context_);
            return;
        }

        memory_[text_++] = found_symbol;
        std::uint32_t next = text_;
        if (text_ >= units_start_) {
            restart();
            return;
        }

        if (f_successor != 0) {
            if (f_successor <= next) {
                const std::uint32_t cs = create_successors(false);
                if (cs == 0) {
                    restart();
                    return;
                }
                f_successor = cs;
            }
            if (--order_fall_ == 0) {
                next = f_successor;
                text_ -= (max_context_ != min_context_);
            }
        } else {
            set_successor(found_state_, next);
            f_successor = min_context_;
        }

        const unsigned ns = num_stats(min_context_);
        const unsigned s0 = summ_freq(min_context_) - ns - (found_freq - 1);

        for (std::uint32_t c = max_context_; c != min_context_; c = suffix(c)) {
            const unsigned ns1 = num_stats(c);
            if (ns1 != 1) {
                if ((ns1 & 1) == 0) {
                    // The stats array is full: grow it by one unit.
                    const unsigned old_nu = ns1 >> 1;
                    const unsigned i = u2i(old_nu);
                    if (i != u2i(old_nu + 1)) {
                        const std::uint32_t ptr = alloc_units(i + 1);
                        if (ptr == 0) {
                            restart();
                            return;
                        }
                        const std::uint32_t old_ptr = stats(c);
                        std::memmove(&memory_[ptr], &memory_[old_ptr], u2b(old_nu));
                        insert_node(old_ptr, i);
                        set_stats(c, ptr);
                    }
                }
                set_summ_freq(c, summ_freq(c) + (2 * ns1 < ns) +
                                     2 * ((4 * ns1 <= ns) & (summ_freq(c) <= 8 * ns1)));
            } else {
                const std::uint32_t s = alloc_units(0);
                if (s == 0) {
                    restart();
                    return;
                }
                copy_state(s, one_state(c));
                set_stats(c, s);
                if (freq(s) < kMaxFreq / 4 - 1) {
                    freq(s) = static_cast<std::uint8_t>(freq(s) << 1);
                } else {
                    freq(s) = kMaxFreq - 4;
                }
                set_summ_freq(c, freq(s) + init_esc_ + (ns > 3));
            }

            std::uint32_t cf = 2 * found_freq * (summ_freq(c) + 6);
            const std::uint32_t sf = s0 + summ_freq(c);
            if (cf < 6 * sf) {
                cf = 1 + (cf > sf) + (cf >= 4 * sf);
                set_summ_freq(c, summ_freq(c) + 3);
            } else {
                cf = 4 + (cf >= 9 * sf) + (cf >= 12 * sf) + (cf >= 15 * sf);
                set_summ_freq(c, summ_freq(c) + cf);
            }
            const std::uint32_t s = stats(c) + ns1 * kStateSize;
            set_successor(s, next);
            memory_[s] = found_symbol;
            freq(s) = static_cast<std::uint8_t>(cf);
            set_num_stats(c, ns1 + 1);
        }
        max_context_ = min_context_ = f_successor;
    }

    void rescale()
    {
        const std::uint32_t first = stats(min_context_);
        std::uint32_t s = found_state_;
        {
            std::uint8_t tmp[kStateSize];
            std::memcpy(tmp, &memory_[s], kStateSize);
            for (; s != first; s -= kStateSize) {
                copy_state(s, s - kStateSize);
            }
            std::memcpy(&memory_[s], tmp, kStateSize);
        }
        unsigned esc_freq = summ_freq(min_context_) - freq(s);
        freq(s) = static_cast<std::uint8_t>(freq(s) + 4);
        const unsigned adder = order_fall_ != 0;
        freq(s) = static_cast<std::uint8_t>((freq(s) + adder) >> 1);
        unsigned sum_freq = freq(s);

        unsigned i = num_stats(min_context_) - 1u;
        do {
            s += kStateSize;
            esc_freq -= freq(s);
            freq(s) = static_cast<std::uint8_t>((freq(s) + adder) >> 1);
            sum_freq += freq(s);
            if (freq(s) > freq(s - kStateSize)) {
                std::uint32_t s1 = s;
                std::uint8_t tmp[kStateSize];
                std::memcpy(tmp, &memory_[s1], kStateSize);
                do {
                    copy_state(s1, s1 - kStateSize);
                    s1 -= kStateSize;
                } while (s1 != first && tmp[1] > freq(s1 - kStateSize));
                std::memcpy(&memory_[s1], tmp, kStateSize);
            }
        } while (--i);

        if (freq(s) == 0) {
            const unsigned old_count = num_stats(min_context_);
            i = 0;
            do {
                ++i;
                s -= kStateSize;
            } while (freq(s) == 0);
            esc_freq += i;
            set_num_stats(min_context_, old_count - i);
            if (num_stats(min_context_) == 1) {
                std::uint8_t tmp[kStateSize];
                std::memcpy(tmp, &memory_[first], kStateSize);
                do {
                    tmp[1] = static_cast<std::uint8_t>(tmp[1] - (tmp[1] >> 1));
                    esc_freq >>= 1;
                } while (esc_freq > 1);
                insert_node(first, u2i((old_count + 1) >> 1));
                found_state_ = one_state(min_context_);
                std::memcpy(&memory_[found_state_], tmp, kStateSize);
                return;
            }
            const unsigned n0 = (old_count + 1) >> 1;
            const unsigned n1 = (num_stats(min_context_) + 1) >> 1;
            if (n0 != n1) {
                set_stats(min_context_, shrink_units(first, n0, n1));
            }
        }
        set_summ_freq(min_context_, sum_freq + esc_freq - (esc_freq >> 1));
        found_state_ = stats(min_context_);
    }

    See& make_esc_freq(unsigned masked, std::uint32_t& esc_freq)
    {
        const unsigned ns = num_stats(min_context_);
        if (ns == 256) {
            esc_freq = 1;
            return dummy_see_;
        }
        const unsigned non_masked = ns - masked;
        See& see = see_[ns_to_index_[non_masked - 1]]
                       [(non_masked < num_stats(suffix(min_context_)) - ns) +
                        2 * (summ_freq(min_context_) < 11 * ns) + 4 * (masked > non_masked) + hi_bits_flag_];
        const unsigned r = see.summ >> see.shift;
        see.summ = static_cast<std::uint16_t>(see.summ - r);
        esc_freq = r + (r == 0);
        return see;
    }

    std::uint16_t& bin_summ()
    {
        const std::uint32_t one = one_state(min_context_);
        hi_bits_flag_ = hb_to_flag_[sym(found_state_)];
        return bin_summ_[freq(one) - 1u][prev_success_ + ns_to_bs_index_[num_stats(suffix(min_context_)) - 1] +
                                         hi_bits_flag_ + 2 * hb_to_flag_[sym(one)] +
                                         ((run_length_ >> 26) & 0x20)];
    }

    void next_context()
    {
        const std::uint32_t c = successor(found_state_);
        if (order_fall_ == 0 && c > text_) {
            min_context_ = max_context_ = c;
        } else {
            update_model();
        }
    }

    void update1()
    {
        std::uint32_t s = found_state_;
        freq(s) = static_cast<std::uint8_t>(freq(s) + 4);
        set_summ_freq(min_context_, summ_freq(min_context_) + 4);
        if (freq(s) > freq(s - kStateSize)) {
            swap_states(s, s - kStateSize);
            found_state_ = s -= kStateSize;
            if (freq(s) > kMaxFreq) {
                rescale();
            }
        }
        next_context();
    }

    void update1_0()
    {
        prev_success_ = 2u * freq(found_state_) > summ_freq(min_context_);
        run_length_ += static_cast<std::int32_t>(prev_success_);
        set_summ_freq(min_context_, summ_freq(min_context_) + 4);
        freq(found_state_) = static_cast<std::uint8_t>(freq(found_state_) + 4);
        if (freq(found_state_) > kMaxFreq) {
            rescale();
        }
        next_context();
    }

    void update_bin()
    {
        if (freq(found_state_) < 128) {
            ++freq(found_state_);
        }
        prev_success_ = 1;
        ++run_length_;
        next_context();
    }

    void update2()
    {
        freq(found_state_) = static_cast<std::uint8_t>(freq(found_state_) + 4);
        set_summ_freq(min_context_, summ_freq(min_context_) + 4);
        if (freq(found_state_) > kMaxFreq) {
            rescale();
        }
        run_length_ = init_rl_;
        update_model();
    }

    std::unique_ptr<std::uint8_t[]> memory_; // never read before written, like the reference
    std::uint32_t size_;
    std::uint32_t align_offset_ = 0;
    unsigned max_order_;

    std::uint32_t min_context_ = 0;
    std::uint32_t max_context_ = 0;
    std::uint32_t found_state_ = 0;
    unsigned order_fall_ = 0;
    unsigned init_esc_ = 0;
    unsigned prev_success_ = 0;
    unsigned hi_bits_flag_ = 0;
    std::int32_t run_length_ = 0;
    std::int32_t init_rl_ = 0;
    std::uint32_t glue_count_ = 0;
    std::uint32_t lo_unit_ = 0;
    std::uint32_t hi_unit_ = 0;
    std::uint32_t text_ = 0;
    std::uint32_t units_start_ = 0;

    std::array<std::uint8_t, kNumIndexes> index_to_units_{};
    std::array<std::uint8_t, 128> units_to_index_{};
    std::uint32_t free_list_[kNumIndexes] = {};
    std::array<std::uint8_t, 256> ns_to_index_{};
    std::array<std::uint8_t, 256> ns_to_bs_index_{};
    std::array<std::uint8_t, 256> hb_to_flag_{};
    See dummy_see_;
    See see_[25][16];
    std::uint16_t bin_summ_[128][64] = {};
};

class Ppmd7Stream final : public io::InputStream {
public:
    Ppmd7Stream(io::StreamPtr upstream, unsigned order, std::uint32_t memory, std::uint64_t size)
        : rc_(std::move(upstream)), model_(memory, order), size_(size)
    {
        rc_.init();
    }

    std::size_t read(std::uint8_t* data, std::size_t size) override
    {
        std::size_t n = 0;
        while (n < size && produced_ < size_) {
            const int symbol = model_.decode_symbol(rc_);
            if (symbol < 0) {
                throw Error(ErrorCode::Corrupt, symbol == -1 ? "ppmd: unexpected end marker" : "ppmd: invalid data");
            }
            data[n++] = static_cast<std::uint8_t>(symbol);
            ++produced_;
        }
        return n;
    }

private:
    RangeDecoder rc_;
    Model model_;
    std::uint64_t size_;
    std::uint64_t produced_ = 0;
};

} // namespace

io::StreamPtr decode_ppmd7(io::StreamPtr upstream, const std::vector<std::uint8_t>& properties, std::uint64_t size,
                           std::uint64_t memory_limit)
{
    if (properties.size() != 5) {
        throw Error(ErrorCode::Corrupt, "ppmd: invalid properties");
    }
    const unsigned order = properties[0];
    const std::uint32_t memory = static_cast<std::uint32_t>(properties[1]) |
                                 (static_cast<std::uint32_t>(properties[2]) << 8) |
                                 (static_cast<std::uint32_t>(properties[3]) << 16) |
                                 (static_cast<std::uint32_t>(properties[4]) << 24);
    if (order < 2 || order > kMaxOrder || memory < (1u << 11) || memory > 0xFFFFFFFFu - 12 * 3) {
        throw Error(ErrorCode::Unsupported, "ppmd: unsupported parameters");
    }
    if (memory_limit != 0 && memory > memory_limit) {
        throw Error(ErrorCode::LimitExceeded,
                    "ppmd: the stream needs " + std::to_string(memory >> 20) + " MiB of memory (limit: max_memory)");
    }
    return std::make_unique<Ppmd7Stream>(std::move(upstream), order, memory, size);
}

} // namespace digzip::codec
