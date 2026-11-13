#include "text.hpp"

namespace digzip::text {

namespace {

void append_utf8(std::string& out, char32_t cp)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Upper half of IBM code page 437 (the original IBM PC character set used by old ZIP files).
constexpr char16_t kCp437High[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE,
    0x00EC, 0x00C4, 0x00C5, 0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6,
    0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA,
    0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB, 0x2591, 0x2592, 0x2593, 0x2502,
    0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510, 0x2514,
    0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550,
    0x256C, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C,
    0x2588, 0x2584, 0x258C, 0x2590, 0x2580, 0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229, 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320,
    0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

} // namespace

bool is_valid_utf8(std::string_view bytes)
{
    std::size_t i = 0;
    while (i < bytes.size()) {
        const auto c = static_cast<unsigned char>(bytes[i]);
        std::size_t length = 0;
        char32_t cp = 0;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            length = 2;
            cp = c & 0x1Fu;
        } else if ((c & 0xF0) == 0xE0) {
            length = 3;
            cp = c & 0x0Fu;
        } else if ((c & 0xF8) == 0xF0) {
            length = 4;
            cp = c & 0x07u;
        } else {
            return false;
        }
        if (i + length > bytes.size()) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(bytes[i + k]);
            if ((next & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (next & 0x3Fu);
        }
        const bool overlong = (length == 2 && cp < 0x80) || (length == 3 && cp < 0x800) ||
                              (length == 4 && cp < 0x10000);
        if (overlong || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += length;
    }
    return true;
}

std::string latin1_to_utf8(std::string_view bytes)
{
    std::string out;
    out.reserve(bytes.size());
    for (const char c : bytes) {
        append_utf8(out, static_cast<unsigned char>(c));
    }
    return out;
}

std::string cp437_to_utf8(std::string_view bytes)
{
    std::string out;
    out.reserve(bytes.size());
    for (const char c : bytes) {
        const auto byte = static_cast<unsigned char>(c);
        append_utf8(out, byte < 0x80 ? char32_t{byte} : char32_t{kCp437High[byte - 0x80]});
    }
    return out;
}

std::string utf16le_to_utf8(const std::uint8_t* data, std::size_t size)
{
    std::string out;
    out.reserve(size / 2);
    std::size_t i = 0;
    while (i + 1 < size) {
        char32_t unit = static_cast<char32_t>(data[i] | (data[i + 1] << 8));
        i += 2;
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < size) {
            const auto low = static_cast<char32_t>(data[i] | (data[i + 1] << 8));
            if (low >= 0xDC00 && low <= 0xDFFF) {
                unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        }
        if (unit >= 0xD800 && unit <= 0xDFFF) {
            unit = 0xFFFD;
        }
        append_utf8(out, unit);
    }
    return out;
}

std::string utf8_to_utf16le(std::string_view utf8)
{
    std::string out;
    out.reserve(utf8.size() * 2);
    const auto put = [&](char32_t unit) {
        out += static_cast<char>(unit & 0xFF);
        out += static_cast<char>(unit >> 8);
    };
    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto c = static_cast<unsigned char>(utf8[i]);
        std::size_t length = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (length == 0 || i + length > utf8.size() || !is_valid_utf8(utf8.substr(i, length))) {
            put(0xFFFD);
            ++i;
            continue;
        }
        char32_t cp = length == 1 ? c : c & (0xFFu >> (length + 1));
        for (std::size_t k = 1; k < length; ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3Fu);
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            put(0xD800 + (cp >> 10));
            put(0xDC00 + (cp & 0x3FF));
        } else {
            put(cp);
        }
        i += length;
    }
    return out;
}

std::size_t utf16_length(std::string_view utf8)
{
    std::size_t units = 0;
    for (const char c : utf8) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte & 0xC0) != 0x80) {
            units += (byte & 0xF8) == 0xF0 ? 2 : 1;
        }
    }
    return units;
}

std::string path_to_utf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::filesystem::path path_from_utf8(std::string_view utf8)
{
#ifdef __cpp_char8_t
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
#else
    return std::filesystem::u8path(utf8.begin(), utf8.end());
#endif
}

std::string to_utf8_or_latin1(std::string_view bytes)
{
    return is_valid_utf8(bytes) ? std::string(bytes) : latin1_to_utf8(bytes);
}

} // namespace digzip::text
