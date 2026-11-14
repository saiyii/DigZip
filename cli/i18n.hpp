#pragma once

#include <digzip/error.hpp>

#include <cstdint>
#include <initializer_list>
#include <string>

namespace cli {

enum class Language { English, French };

// DIGZIP_LANG ("en" or "fr") when set, otherwise the language of the user's system;
// English for any other language.
Language detect_language();
void set_language(Language language);
Language language();

// The translation of an English message into the current language, or the message itself.
const char* tr(const char* english);

// `text` with {1}, {2}... replaced by the arguments.
std::string fill(std::string text, std::initializer_list<std::string> args);

// "1 file", "3 files" in the current language; `one` and `many` are English and translated.
std::string counted(std::uint64_t count, const char* one, const char* many);

// An engine message as shown to the user: as is in English, after a translated description of
// its code otherwise (engine messages are in English).
std::string describe(digzip::ErrorCode code, const std::string& message);

// Text padded with spaces to `width` characters (not bytes).
std::string pad_right(const std::string& text, std::size_t width);
std::string pad_left(const std::string& text, std::size_t width);

} // namespace cli
