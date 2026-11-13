#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace digzip::text {

bool is_valid_utf8(std::string_view bytes);

// Legacy 8-bit encodings, converted byte by byte.
std::string latin1_to_utf8(std::string_view bytes);
std::string cp437_to_utf8(std::string_view bytes);

// Little-endian UTF-16; unpaired surrogates become U+FFFD.
std::string utf16le_to_utf8(const std::uint8_t* data, std::size_t size);

// Number of UTF-16 code units needed for valid UTF-8 text.
std::size_t utf16_length(std::string_view utf8);

// Keeps valid UTF-8 as is, otherwise decodes `bytes` as Latin-1.
std::string to_utf8_or_latin1(std::string_view bytes);

} // namespace digzip::text
