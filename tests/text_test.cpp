#include "text.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace digzip::text;

TEST_CASE("UTF-8 validation")
{
    CHECK(is_valid_utf8("plain ascii"));
    CHECK(is_valid_utf8("caf\xC3\xA9 \xE2\x98\x95 \xF0\x9F\x93\xA6"));
    CHECK_FALSE(is_valid_utf8("\xC3"));             // truncated
    CHECK_FALSE(is_valid_utf8("\xC0\xAF"));         // overlong '/'
    CHECK_FALSE(is_valid_utf8("\xED\xA0\x80"));     // surrogate
    CHECK_FALSE(is_valid_utf8("\xF4\x90\x80\x80")); // above U+10FFFF
    CHECK_FALSE(is_valid_utf8("\xE9t\xE9"));        // Latin-1
}

TEST_CASE("legacy encodings convert to UTF-8")
{
    CHECK(latin1_to_utf8("\xE9t\xE9") == "\xC3\xA9t\xC3\xA9");
    CHECK(cp437_to_utf8("\x82t\x82") == "\xC3\xA9t\xC3\xA9");
    CHECK(cp437_to_utf8("\x9C") == "\xC2\xA3");
    CHECK(to_utf8_or_latin1("d\xE9j\xE0") == "d\xC3\xA9j\xC3\xA0");
    CHECK(to_utf8_or_latin1("d\xC3\xA9j\xC3\xA0") == "d\xC3\xA9j\xC3\xA0");
}

TEST_CASE("UTF-16 decoding and length")
{
    const std::uint8_t text[] = {'a', 0, 0xE9, 0, 0x3D, 0xD8, 0x00, 0xDE, 0x00, 0xD8};
    CHECK(utf16le_to_utf8(text, sizeof text) == "a\xC3\xA9\xF0\x9F\x98\x80\xEF\xBF\xBD");
    CHECK(utf16_length("a\xC3\xA9\xF0\x9F\x98\x80") == 4);
}
