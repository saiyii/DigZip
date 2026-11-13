#pragma once

#include <atomic>
#include <optional>
#include <string>

namespace cli {

// Console in UTF-8 on Windows; nothing to do elsewhere.
void setup_console();

bool stderr_is_terminal();
unsigned terminal_width();

// Environment variable in UTF-8, if set.
std::optional<std::string> environment(const std::string& name);

// Sets the returned flag on the first Ctrl+C (or Ctrl+Break, SIGTERM); a second one ends the process at once.
const std::atomic<bool>& install_interrupt_handler();

#ifdef _WIN32
std::string to_utf8(const wchar_t* text);
#endif

} // namespace cli
