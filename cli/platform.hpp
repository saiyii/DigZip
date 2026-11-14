#pragma once

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace cli {

// Paths to and from UTF-8, in C++17 and C++20 alike.
std::string path_to_utf8(const std::filesystem::path& path);
std::filesystem::path path_from_utf8(std::string_view utf8);

// Console in UTF-8 on Windows; nothing to do elsewhere.
void setup_console();

bool stderr_is_terminal();
unsigned terminal_width();

// Environment variable in UTF-8, if set.
std::optional<std::string> environment(const std::string& name);

// Two-letter code of the user's language ("fr", "en"...), or empty if unknown.
std::string system_language();

// Sets the returned flag on the first Ctrl+C (or Ctrl+Break, SIGTERM); a second one ends the process at once.
const std::atomic<bool>& install_interrupt_handler();

#ifdef _WIN32
std::string to_utf8(const wchar_t* text);
#endif

} // namespace cli
