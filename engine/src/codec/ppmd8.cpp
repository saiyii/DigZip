// PPMd variant I revision 1 decoder (ZIP method 98), written for DigZip from Dmitry Shkarin's
// public PPMd algorithm. Same principle as variant H (see ppmd7.cpp), with a carry-less range
// coder, per-context flags, and two ways to recover when the model memory is full: restart the
// model, or cut off the least useful branches of the context tree.

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
constexpr std::uint32_t kTop = 1u << 24;
constexpr std::uint32_t kBot = 1u << 15;
constexpr std::uint32_t kStateSize = 6;
constexpr std::uint32_t kEmptyNode = 0xFFFFFFFF;

constexpr unsigned kRestoreRestart = 0;
constexpr unsigned kRestoreCutOff = 1;

constexpr std::uint16_t kInitBinEsc[8] = {0x3CDD, 0x1F3F, 0x59BF, 0x48F3, 0x64A1, 0x5ABC, 0x6632, 0x6051};
constexpr std::uint8_t kExpEscape[16] = {25, 14, 9, 7, 5, 5, 4, 4, 4, 3, 3, 3, 2, 2, 2, 2};

unsigned get_mean(unsigned prob)
{
    return (prob + (1u << (kPeriodBits - 2))) >> kPeriodBits;
}

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

// Carry-less range decoder (Subbotin), as used by PPMd variant I.
class RangeDecoder {
public:
    explicit RangeDecoder(io::StreamPtr upstream) : in_(std::move(upstream)) {}

    void init()
    {
        for (int i = 0; i < 4; ++i) {
            code_ = (code_ << 8) | byte();
        }
        if (code_ == 0xFFFFFFFF) {
            throw Error(ErrorCode::Corrupt, "ppmd: invalid stream start");
        }
    }

    std::uint32_t threshold(std::uint32_t total) { return code_ / (range_ /= total); }
    std::uint32_t binary_threshold() { return code_ / (range_ >>= 14); }

    void decode(std::uint32_t start, std::uint32_t size)
    {
        start *= range_;
        low_ += start;
        code_ -= start;
        range_ *= size;
        for (;;) {
            if ((low_ ^ (low_ + range_)) >= kTop) {
                if (range_ >= kBot) {
                    break;
                }
                range_ = (0 - low_) & (kBot - 1);
            }
            code_ = (code_ << 8) | byte();
            range_ <<= 8;
            low_ <<= 8;
        }
    }

private:
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

    InputBuffer in_;
    std::uint32_t range_ = 0xFFFFFFFF;
    std::uint32_t code_ = 0;
    std::uint32_t low_ = 0;
    unsigned overrun_ = 0;
};

class Model {
public:
    Model(std::uint32_t size, unsigned max_order, unsigned restore_method)
        : size_(size), max_order_(max_order), restore_method_(restore_method)
    {
        align_offset_ = 4 - (size & 3);
        memory_.reset(new std::uint8_t[std::size_t{align_offset_} + size]);

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
        for (; i < 5; ++i) {
            ns_to_index_[i] = static_cast<std::uint8_t>(i);
        }
        for (unsigned m = i, k = 1; i < 260; ++i) {
            ns_to_index_[i] = static_cast<std::uint8_t>(m);
            if (--k == 0) {
                k = ++m - 4;
            }
        }

        restart();
        dummy_see_.shift = kPeriodBits;
        dummy_see_.summ = 0;
        dummy_see_.count = 64;
    }

    // Returns the symbol, -1 at the end marker, -2 on invalid data.
    int decode_symbol(RangeDecoder& rc)
    {
        std::array<std::int8_t, 256> mask;
        if (num_stats(min_context_) != 0) {
            std::uint32_t s = stats(min_context_);
            const std::uint32_t count = rc.threshold(summ_freq(min_context_));
            std::uint32_t high = freq(s);
            if (count < high) {
                rc.decode(0, high);
                found_state_ = s;
                const int symbol = sym(s);
                update1_0();
                return symbol;
            }
            prev_success_ = 0;
            unsigned i = num_stats(min_context_);
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
            rc.decode(high, summ_freq(min_context_) - high);
            mask.fill(-1);
            mask[sym(s)] = 0;
            i = num_stats(min_context_);
            do {
                s -= kStateSize;
                mask[sym(s)] = 0;
            } while (--i);
        } else {
            std::uint16_t& prob = bin_summ();
            if (rc.binary_threshold() < prob) {
                rc.decode(0, prob);
                prob = static_cast<std::uint16_t>(prob + (1u << kIntBits) - get_mean(prob));
                found_state_ = one_state(min_context_);
                const int symbol = sym(found_state_);
                update_bin();
                return symbol;
            }
            rc.decode(prob, kBinScale - prob);
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
    void copy_units(std::uint32_t to, std::uint32_t from, unsigned units)
    {
        std::memmove(&memory_[to], &memory_[from], units * kUnitSize);
    }

    // State: Symbol, Freq, Successor (32 bits, unaligned). 6 bytes.
    std::uint8_t sym(std::uint32_t s) const { return memory_[s]; }
    std::uint8_t& freq(std::uint32_t s) { return memory_[s + 1]; }
    std::uint32_t successor(std::uint32_t s) const { return rd32(s + 2); }
    void set_successor(std::uint32_t s, std::uint32_t v) { wr32(s + 2, v); }
    void copy_state(std::uint32_t to, std::uint32_t from) { std::memmove(&memory_[to], &memory_[from], kStateSize); }
    void swap_states(std::uint32_t a, std::uint32_t b)
    {
        std::uint8_t tmp[kStateSize];
        std::memcpy(tmp, &memory_[a], kStateSize);
        std::memcpy(&memory_[a], &memory_[b], kStateSize);
        std::memcpy(&memory_[b], tmp, kStateSize);
    }

    // Context: NumStats - 1 (8), Flags (8), SummFreq (16), Stats (32), Suffix (32). 12 bytes.
    unsigned num_stats(std::uint32_t c) const { return memory_[c]; }
    void set_num_stats(std::uint32_t c, unsigned v) { memory_[c] = static_cast<std::uint8_t>(v); }
    std::uint8_t& flags(std::uint32_t c) { return memory_[c + 1]; }
    unsigned summ_freq(std::uint32_t c) const { return rd16(c + 2); }
    void set_summ_freq(std::uint32_t c, unsigned v) { wr16(c + 2, v); }
    std::uint32_t stats(std::uint32_t c) const { return rd32(c + 4); }
    void set_stats(std::uint32_t c, std::uint32_t v) { wr32(c + 4, v); }
    std::uint32_t suffix(std::uint32_t c) const { return rd32(c + 8); }
    void set_suffix(std::uint32_t c, std::uint32_t v) { wr32(c + 8, v); }
    static std::uint32_t one_state(std::uint32_t c) { return c + 2; }

    // Free block: Stamp (32), Next (32), NU (32).
    std::uint32_t node_stamp(std::uint32_t n) const { return rd32(n); }
    std::uint32_t node_next(std::uint32_t n) const { return rd32(n + 4); }
    std::uint32_t node_nu(std::uint32_t n) const { return rd32(n + 8); }

    // --- sub-allocator --------------------------------------------------------------------

    unsigned i2u(unsigned index) const { return index_to_units_[index]; }
    unsigned u2i(unsigned units) const { return units_to_index_[units - 1]; }
    static std::uint32_t u2b(unsigned units) { return units * kUnitSize; }

    void insert_node(std::uint32_t node, unsigned index)
    {
        wr32(node, kEmptyNode);
        wr32(node + 4, free_list_[index]);
        wr32(node + 8, i2u(index));
        free_list_[index] = node;
        ++stamps_[index];
    }

    std::uint32_t remove_node(unsigned index)
    {
        const std::uint32_t node = free_list_[index];
        free_list_[index] = node_next(node);
        --stamps_[index];
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
        std::uint32_t head = 0;
        std::uint32_t prev_link = 0; // offset of the "Next" field to patch; 0 means `head`
        const auto link = [&](std::uint32_t value) {
            if (prev_link == 0) {
                head = value;
            } else {
                wr32(prev_link, value);
            }
        };

        glue_count_ = 1 << 13;
        std::fill(std::begin(stamps_), std::end(stamps_), 0u);
        // The order-0 context always holds the top unit, so only LoUnit needs a guard.
        if (lo_unit_ != hi_unit_) {
            wr32(lo_unit_, 0);
        }

        for (unsigned i = 0; i < kNumIndexes; ++i) {
            std::uint32_t next = free_list_[i];
            free_list_[i] = 0;
            while (next != 0) {
                const std::uint32_t node = next;
                if (node_nu(node) != 0) {
                    link(node);
                    prev_link = node + 4;
                    for (std::uint32_t node2; node_stamp(node2 = node + node_nu(node) * kUnitSize) == kEmptyNode;) {
                        wr32(node + 8, node_nu(node) + node_nu(node2));
                        wr32(node2 + 8, 0);
                    }
                }
                next = node_next(node);
            }
        }
        link(0);

        while (head != 0) {
            std::uint32_t node = head;
            head = node_next(node);
            unsigned nu = node_nu(node);
            if (nu == 0) {
                continue;
            }
            for (; nu > 128; nu -= 128, node += 128 * kUnitSize) {
                insert_node(node, kNumIndexes - 1);
            }
            unsigned i = u2i(nu);
            if (i2u(i) != nu) {
                const unsigned k = i2u(--i);
                insert_node(node + k * kUnitSize, nu - k - 1);
            }
            insert_node(node, i);
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
            copy_units(ptr, old_ptr, new_nu);
            insert_node(old_ptr, i0);
            return ptr;
        }
        split_block(old_ptr, i0, i1);
        return old_ptr;
    }

    void free_units(std::uint32_t ptr, unsigned nu) { insert_node(ptr, u2i(nu)); }

    void special_free_unit(std::uint32_t ptr)
    {
        if (ptr != units_start_) {
            insert_node(ptr, 0);
        } else {
            units_start_ += kUnitSize;
        }
    }

    std::uint32_t move_units_up(std::uint32_t old_ptr, unsigned nu)
    {
        const unsigned index = u2i(nu);
        if (old_ptr > units_start_ + 16 * 1024 || old_ptr > free_list_[index]) {
            return old_ptr;
        }
        const std::uint32_t ptr = remove_node(index);
        copy_units(ptr, old_ptr, nu);
        if (old_ptr != units_start_) {
            insert_node(old_ptr, index);
        } else {
            units_start_ += u2b(i2u(index));
        }
        return ptr;
    }

    void expand_text_area()
    {
        std::uint32_t count[kNumIndexes] = {};
        if (lo_unit_ != hi_unit_) {
            wr32(lo_unit_, 0);
        }
        std::uint32_t node = units_start_;
        for (; node_stamp(node) == kEmptyNode; node += node_nu(node) * kUnitSize) {
            wr32(node, 0);
            ++count[u2i(node_nu(node))];
        }
        units_start_ = node;

        for (unsigned i = 0; i < kNumIndexes; ++i) {
            // `link` is the offset of the field holding the next reference: the list head or a node's Next.
            std::uint32_t* head = &free_list_[i];
            std::uint32_t link = 0;
            const auto get = [&] { return link == 0 ? *head : rd32(link); };
            const auto set = [&](std::uint32_t v) {
                if (link == 0) {
                    *head = v;
                } else {
                    wr32(link, v);
                }
            };
            while (count[i] != 0) {
                std::uint32_t n = get();
                while (node_stamp(n) == 0) {
                    set(node_next(n));
                    n = get();
                    --stamps_[i];
                    if (--count[i] == 0) {
                        break;
                    }
                }
                link = n + 4;
            }
        }
    }

    std::uint32_t used_memory() const
    {
        std::uint32_t units = 0;
        for (unsigned i = 0; i < kNumIndexes; ++i) {
            units += stamps_[i] * i2u(i);
        }
        return size_ - (hi_unit_ - lo_unit_) - (units_start_ - text_) - u2b(units);
    }

    // --- model ----------------------------------------------------------------------------

    void restart()
    {
        std::fill(std::begin(free_list_), std::end(free_list_), 0u);
        std::fill(std::begin(stamps_), std::end(stamps_), 0u);
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
        set_num_stats(min_context_, 255);
        flags(min_context_) = 0;
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

        for (unsigned i = 0, m = 0; m < 25; ++m) {
            while (ns_to_index_[i] == m) {
                ++i;
            }
            for (unsigned k = 0; k < 8; ++k) {
                const auto value = static_cast<std::uint16_t>(kBinScale - kInitBinEsc[k] / (i + 1));
                for (unsigned r = 0; r < 64; r += 8) {
                    bin_summ_[m][k + r] = value;
                }
            }
        }
        for (unsigned i = 0, m = 0; m < 24; ++m) {
            while (ns_to_index_[i + 3] == m + 3) {
                ++i;
            }
            for (See& s : see_[m]) {
                s.shift = kPeriodBits - 4;
                s.summ = static_cast<std::uint16_t>((2 * i + 5) << s.shift);
                s.count = 7;
            }
        }
    }

    // Rescales the statistics of a context after its stats array was shrunk to fit.
    void refresh(std::uint32_t c, unsigned old_nu, unsigned scale)
    {
        unsigned i = num_stats(c);
        std::uint32_t s = shrink_units(stats(c), old_nu, (i + 2) >> 1);
        set_stats(c, s);
        unsigned f = (flags(c) & (0x10 + 0x04 * scale)) + 0x08 * (sym(s) >= 0x40);
        unsigned esc_freq = summ_freq(c) - freq(s);
        freq(s) = static_cast<std::uint8_t>((freq(s) + scale) >> scale);
        unsigned sum_freq = freq(s);
        do {
            s += kStateSize;
            esc_freq -= freq(s);
            freq(s) = static_cast<std::uint8_t>((freq(s) + scale) >> scale);
            sum_freq += freq(s);
            f |= 0x08 * (sym(s) >= 0x40);
        } while (--i);
        set_summ_freq(c, sum_freq + ((esc_freq + scale) >> scale));
        flags(c) = static_cast<std::uint8_t>(f);
    }

    std::uint32_t cut_off(std::uint32_t c, unsigned order)
    {
        if (num_stats(c) == 0) {
            const std::uint32_t s = one_state(c);
            if (successor(s) >= units_start_) {
                set_successor(s, order < max_order_ ? cut_off(successor(s), order + 1) : 0);
                if (successor(s) != 0 || order <= 9) {
                    return c;
                }
            }
            special_free_unit(c);
            return 0;
        }

        const unsigned units = (num_stats(c) + 2) >> 1;
        set_stats(c, move_units_up(stats(c), units));

        int i = static_cast<int>(num_stats(c));
        for (std::uint32_t s = stats(c) + static_cast<std::uint32_t>(i) * kStateSize;; s -= kStateSize) {
            if (successor(s) < units_start_) {
                const std::uint32_t s2 = stats(c) + static_cast<std::uint32_t>(i--) * kStateSize;
                set_successor(s, 0);
                swap_states(s, s2);
            } else if (order < max_order_) {
                set_successor(s, cut_off(successor(s), order + 1));
            } else {
                set_successor(s, 0);
            }
            if (s == stats(c)) {
                break;
            }
        }

        if (i != static_cast<int>(num_stats(c)) && order != 0) {
            set_num_stats(c, static_cast<unsigned>(i));
            const std::uint32_t s = stats(c);
            if (i < 0) {
                free_units(s, units);
                special_free_unit(c);
                return 0;
            }
            if (i == 0) {
                flags(c) = static_cast<std::uint8_t>((flags(c) & 0x10) + 0x08 * (sym(s) >= 0x40));
                copy_state(one_state(c), s);
                free_units(s, units);
                freq(one_state(c)) = static_cast<std::uint8_t>((freq(one_state(c)) + 11u) >> 3);
            } else {
                refresh(c, units, summ_freq(c) > 16u * static_cast<unsigned>(i));
            }
        }
        return c;
    }

    void restore_model(std::uint32_t c1)
    {
        text_ = align_offset_;
        std::uint32_t c = max_context_;
        for (; c != c1; c = suffix(c)) {
            set_num_stats(c, num_stats(c) - 1);
            if (num_stats(c) == 0) {
                const std::uint32_t s = stats(c);
                flags(c) = static_cast<std::uint8_t>((flags(c) & 0x10) + 0x08 * (sym(s) >= 0x40));
                copy_state(one_state(c), s);
                special_free_unit(s);
                freq(one_state(c)) = static_cast<std::uint8_t>((freq(one_state(c)) + 11u) >> 3);
            } else {
                refresh(c, (num_stats(c) + 3) >> 1, 0);
            }
        }
        for (; c != min_context_; c = suffix(c)) {
            if (num_stats(c) == 0) {
                const std::uint32_t s = one_state(c);
                freq(s) = static_cast<std::uint8_t>(freq(s) - (freq(s) >> 1));
            } else {
                set_summ_freq(c, summ_freq(c) + 4);
                if (summ_freq(c) > 128 + 4 * num_stats(c)) {
                    refresh(c, (num_stats(c) + 2) >> 1, 1);
                }
            }
        }

        if (restore_method_ == kRestoreRestart || used_memory() < (size_ >> 1)) {
            restart();
        } else {
            while (suffix(max_context_) != 0) {
                max_context_ = suffix(max_context_);
            }
            do {
                cut_off(max_context_, 0);
                expand_text_area();
            } while (used_memory() > 3 * (size_ >> 2));
            glue_count_ = 0;
            order_fall_ = max_order_;
        }
    }

    std::uint32_t create_successors(bool skip, std::uint32_t s1, std::uint32_t c)
    {
        const std::uint32_t up_branch = successor(found_state_);
        std::uint32_t states[kMaxOrder + 1];
        unsigned count = 0;
        if (!skip) {
            states[count++] = found_state_;
        }
        while (suffix(c) != 0) {
            c = suffix(c);
            std::uint32_t s;
            if (s1 != 0) {
                s = s1;
                s1 = 0;
            } else if (num_stats(c) != 0) {
                for (s = stats(c); sym(s) != sym(found_state_); s += kStateSize) {
                }
                if (freq(s) < kMaxFreq - 9) {
                    ++freq(s);
                    set_summ_freq(c, summ_freq(c) + 1);
                }
            } else {
                s = one_state(c);
                freq(s) = static_cast<std::uint8_t>(freq(s) + (num_stats(suffix(c)) == 0 && freq(s) < 24 ? 1 : 0));
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
        const auto up_flags =
            static_cast<std::uint8_t>(0x10 * (sym(found_state_) >= 0x40) + 0x08 * (up_symbol >= 0x40));
        unsigned up_freq;
        if (num_stats(c) == 0) {
            up_freq = freq(one_state(c));
        } else {
            std::uint32_t s;
            for (s = stats(c); sym(s) != up_symbol; s += kStateSize) {
            }
            const unsigned cf = freq(s) - 1u;
            const unsigned s0 = summ_freq(c) - num_stats(c) - cf;
            up_freq = 1 + ((2 * cf <= s0) ? (5 * cf > s0) : ((cf + 2 * s0 - 3) / s0));
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
            set_num_stats(c1, 0);
            flags(c1) = up_flags;
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

    std::uint32_t reduce_order(std::uint32_t s1, std::uint32_t c)
    {
        std::uint32_t s = 0;
        const std::uint32_t c1 = c;
        const std::uint32_t up_branch = text_;
        set_successor(found_state_, up_branch);
        ++order_fall_;

        for (;;) {
            if (s1 != 0) {
                c = suffix(c);
                s = s1;
                s1 = 0;
            } else {
                if (suffix(c) == 0) {
                    return c;
                }
                c = suffix(c);
                if (num_stats(c) != 0) {
                    s = stats(c);
                    while (sym(s) != sym(found_state_)) {
                        s += kStateSize;
                    }
                    if (freq(s) < kMaxFreq - 9) {
                        freq(s) = static_cast<std::uint8_t>(freq(s) + 2);
                        set_summ_freq(c, summ_freq(c) + 2);
                    }
                } else {
                    s = one_state(c);
                    freq(s) = static_cast<std::uint8_t>(freq(s) + (freq(s) < 32));
                }
            }
            if (successor(s) != 0) {
                break;
            }
            set_successor(s, up_branch);
            ++order_fall_;
        }

        if (successor(s) <= up_branch) {
            const std::uint32_t saved = found_state_;
            found_state_ = s;
            const std::uint32_t created = create_successors(false, 0, c);
            set_successor(s, created);
            found_state_ = saved;
        }
        if (order_fall_ == 1 && c1 == max_context_) {
            set_successor(found_state_, successor(s));
            --text_;
        }
        return successor(s);
    }

    void update_model()
    {
        std::uint32_t f_successor = successor(found_state_);
        const unsigned f_freq = freq(found_state_);
        const std::uint8_t f_symbol = sym(found_state_);
        std::uint32_t s = 0;

        if (f_freq < kMaxFreq / 4 && suffix(min_context_) != 0) {
            const std::uint32_t c = suffix(min_context_);
            if (num_stats(c) == 0) {
                s = one_state(c);
                if (freq(s) < 32) {
                    ++freq(s);
                }
            } else {
                s = stats(c);
                if (sym(s) != f_symbol) {
                    do {
                        s += kStateSize;
                    } while (sym(s) != f_symbol);
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

        std::uint32_t c = max_context_;
        if (order_fall_ == 0 && f_successor != 0) {
            const std::uint32_t cs = create_successors(true, s, min_context_);
            if (cs == 0) {
                set_successor(found_state_, 0);
                restore_model(c);
            } else {
                set_successor(found_state_, cs);
                max_context_ = cs;
            }
            return;
        }

        memory_[text_++] = f_symbol;
        std::uint32_t next = text_;
        if (text_ >= units_start_) {
            restore_model(c);
            return;
        }

        if (f_successor == 0) {
            const std::uint32_t cs = reduce_order(s, min_context_);
            if (cs == 0) {
                restore_model(c);
                return;
            }
            f_successor = cs;
        } else if (f_successor < units_start_) {
            const std::uint32_t cs = create_successors(false, s, min_context_);
            if (cs == 0) {
                restore_model(c);
                return;
            }
            f_successor = cs;
        }

        if (--order_fall_ == 0) {
            next = f_successor;
            text_ -= (max_context_ != min_context_);
        }

        const unsigned ns = num_stats(min_context_);
        const unsigned s0 = summ_freq(min_context_) - ns - f_freq;
        const auto flag = static_cast<std::uint8_t>(0x08 * (f_symbol >= 0x40));

        for (; c != min_context_; c = suffix(c)) {
            const unsigned ns1 = num_stats(c);
            if (ns1 != 0) {
                if ((ns1 & 1) != 0) {
                    const unsigned old_nu = (ns1 + 1) >> 1;
                    const unsigned i = u2i(old_nu);
                    if (i != u2i(old_nu + 1)) {
                        const std::uint32_t ptr = alloc_units(i + 1);
                        if (ptr == 0) {
                            restore_model(c);
                            return;
                        }
                        const std::uint32_t old_ptr = stats(c);
                        copy_units(ptr, old_ptr, old_nu);
                        insert_node(old_ptr, i);
                        set_stats(c, ptr);
                    }
                }
                set_summ_freq(c, summ_freq(c) + (3 * ns1 + 1 < ns));
            } else {
                const std::uint32_t s2 = alloc_units(0);
                if (s2 == 0) {
                    restore_model(c);
                    return;
                }
                copy_state(s2, one_state(c));
                set_stats(c, s2);
                if (freq(s2) < kMaxFreq / 4 - 1) {
                    freq(s2) = static_cast<std::uint8_t>(freq(s2) << 1);
                } else {
                    freq(s2) = kMaxFreq - 4;
                }
                set_summ_freq(c, freq(s2) + init_esc_ + (ns > 2));
            }

            std::uint32_t cf = 2 * f_freq * (summ_freq(c) + 6);
            const std::uint32_t sf = s0 + summ_freq(c);
            if (cf < 6 * sf) {
                cf = 1 + (cf > sf) + (cf >= 4 * sf);
                set_summ_freq(c, summ_freq(c) + 4);
            } else {
                cf = 4 + (cf > 9 * sf) + (cf > 12 * sf) + (cf > 15 * sf);
                set_summ_freq(c, summ_freq(c) + cf);
            }
            const std::uint32_t s2 = stats(c) + (ns1 + 1) * kStateSize;
            set_successor(s2, next);
            memory_[s2] = f_symbol;
            freq(s2) = static_cast<std::uint8_t>(cf);
            flags(c) = static_cast<std::uint8_t>(flags(c) | flag);
            set_num_stats(c, ns1 + 1);
        }
        max_context_ = min_context_ = f_successor;
    }

    void rescale()
    {
        const std::uint32_t first = stats(min_context_);
        std::uint32_t s = found_state_;
        if (s != first) {
            std::uint8_t tmp[kStateSize];
            std::memcpy(tmp, &memory_[s], kStateSize);
            do {
                copy_state(s, s - kStateSize);
                s -= kStateSize;
            } while (s != first);
            std::memcpy(&memory_[s], tmp, kStateSize);
        }
        unsigned esc_freq = summ_freq(min_context_) - freq(s);
        freq(s) = static_cast<std::uint8_t>(freq(s) + 4);
        const unsigned adder = order_fall_ != 0;
        freq(s) = static_cast<std::uint8_t>((freq(s) + adder) >> 1);
        unsigned sum_freq = freq(s);

        unsigned i = num_stats(min_context_);
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
            if (num_stats(min_context_) == 0) {
                std::uint8_t tmp[kStateSize];
                std::memcpy(tmp, &memory_[first], kStateSize);
                tmp[1] = static_cast<std::uint8_t>((2 * tmp[1] + esc_freq - 1) / esc_freq);
                if (tmp[1] > kMaxFreq / 3) {
                    tmp[1] = kMaxFreq / 3;
                }
                insert_node(first, u2i((old_count + 2) >> 1));
                flags(min_context_) =
                    static_cast<std::uint8_t>((flags(min_context_) & 0x10) + 0x08 * (tmp[0] >= 0x40));
                found_state_ = one_state(min_context_);
                std::memcpy(&memory_[found_state_], tmp, kStateSize);
                return;
            }
            const unsigned n0 = (old_count + 2) >> 1;
            const unsigned n1 = (num_stats(min_context_) + 2) >> 1;
            if (n0 != n1) {
                set_stats(min_context_, shrink_units(first, n0, n1));
            }
            unsigned f = flags(min_context_) & ~0x08u;
            s = stats(min_context_);
            f |= 0x08 * (sym(s) >= 0x40);
            i = num_stats(min_context_);
            do {
                s += kStateSize;
                f |= 0x08 * (sym(s) >= 0x40);
            } while (--i);
            flags(min_context_) = static_cast<std::uint8_t>(f);
        }
        set_summ_freq(min_context_, sum_freq + esc_freq - (esc_freq >> 1));
        flags(min_context_) = static_cast<std::uint8_t>(flags(min_context_) | 0x04);
        found_state_ = stats(min_context_);
    }

    See& make_esc_freq(unsigned masked, std::uint32_t& esc_freq)
    {
        const unsigned ns = num_stats(min_context_);
        if (ns == 0xFF) {
            esc_freq = 1;
            return dummy_see_;
        }
        See& see = see_[ns_to_index_[ns + 2] - 3u]
                       [(summ_freq(min_context_) > 11 * (ns + 1)) +
                        2 * (2 * ns < num_stats(suffix(min_context_)) + masked) + flags(min_context_)];
        const unsigned r = see.summ >> see.shift;
        see.summ = static_cast<std::uint16_t>(see.summ - r);
        esc_freq = r + (r == 0);
        return see;
    }

    std::uint16_t& bin_summ()
    {
        const std::uint32_t one = one_state(min_context_);
        return bin_summ_[ns_to_index_[freq(one) - 1u]][ns_to_bs_index_[num_stats(suffix(min_context_))] +
                                                     prev_success_ + flags(min_context_) +
                                                     ((run_length_ >> 26) & 0x20)];
    }

    void next_context()
    {
        const std::uint32_t c = successor(found_state_);
        if (order_fall_ == 0 && c >= units_start_) {
            min_context_ = max_context_ = c;
        } else {
            update_model();
            min_context_ = max_context_;
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
        prev_success_ = 2u * freq(found_state_) >= summ_freq(min_context_);
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
        freq(found_state_) = static_cast<std::uint8_t>(freq(found_state_) + (freq(found_state_) < 196));
        prev_success_ = 1;
        ++run_length_;
        next_context();
    }

    void update2()
    {
        set_summ_freq(min_context_, summ_freq(min_context_) + 4);
        freq(found_state_) = static_cast<std::uint8_t>(freq(found_state_) + 4);
        if (freq(found_state_) > kMaxFreq) {
            rescale();
        }
        run_length_ = init_rl_;
        update_model();
        min_context_ = max_context_;
    }

    std::unique_ptr<std::uint8_t[]> memory_; // never read before written, like the reference
    std::uint32_t size_;
    std::uint32_t align_offset_ = 0;
    unsigned max_order_;
    unsigned restore_method_;

    std::uint32_t min_context_ = 0;
    std::uint32_t max_context_ = 0;
    std::uint32_t found_state_ = 0;
    unsigned order_fall_ = 0;
    unsigned init_esc_ = 0;
    unsigned prev_success_ = 0;
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
    std::uint32_t stamps_[kNumIndexes] = {};
    std::array<std::uint8_t, 256> ns_to_bs_index_{};
    std::array<std::uint8_t, 260> ns_to_index_{};
    See dummy_see_;
    See see_[24][32];
    std::uint16_t bin_summ_[25][64] = {};
};

class Ppmd8Stream final : public io::InputStream {
public:
    Ppmd8Stream(io::StreamPtr upstream, unsigned order, std::uint32_t memory, unsigned restore, std::uint64_t size)
        : rc_(std::move(upstream)), model_(memory, order, restore), size_(size)
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

io::StreamPtr decode_ppmd8_zip(io::StreamPtr upstream, std::uint64_t size, std::uint64_t memory_limit)
{
    std::uint8_t header[2];
    upstream->read_exact(header, sizeof header);
    const auto value = static_cast<unsigned>(header[0] | (header[1] << 8));
    const unsigned order = (value & 0x0F) + 1;
    const std::uint32_t memory = (((value >> 4) & 0xFF) + 1) << 20;
    const unsigned restore = value >> 12;
    if (order < 2 || restore > kRestoreCutOff) {
        throw Error(ErrorCode::Unsupported, "ppmd: unsupported parameters");
    }
    if (memory_limit != 0 && memory > memory_limit) {
        throw Error(ErrorCode::LimitExceeded,
                    "ppmd: the stream needs " + std::to_string(memory >> 20) + " MiB of memory (limit: max_memory)");
    }
    return std::make_unique<Ppmd8Stream>(std::move(upstream), order, memory, restore, size);
}

} // namespace digzip::codec
