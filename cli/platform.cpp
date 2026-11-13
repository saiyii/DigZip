#include "platform.hpp"

#include <csignal>
#include <cstdlib>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <io.h>
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace cli {

namespace {

std::atomic<bool> g_interrupted{false};

extern "C" void on_interrupt(int signal)
{
    g_interrupted.store(true);
    std::signal(signal, SIG_DFL);
}

} // namespace

void setup_console()
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
}

bool stderr_is_terminal()
{
#ifdef _WIN32
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(STDERR_FILENO) != 0;
#endif
}

unsigned terminal_width()
{
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_ERROR_HANDLE), &info)) {
        return static_cast<unsigned>(info.srWindow.Right - info.srWindow.Left + 1);
    }
#else
    winsize size{};
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
        return size.ws_col;
    }
#endif
    return 80;
}

#ifdef _WIN32
std::string to_utf8(const wchar_t* text)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    out.pop_back(); // terminating NUL
    return out;
}

std::optional<std::string> environment(const std::string& name)
{
    const int size = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wide.data(), size);
    wchar_t* value = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&value, &length, wide.c_str()) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string utf8 = to_utf8(value);
    std::free(value);
    return utf8;
}
#else
std::optional<std::string> environment(const std::string& name)
{
    const char* value = std::getenv(name.c_str());
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
}
#endif

const std::atomic<bool>& install_interrupt_handler()
{
    static_assert(std::atomic<bool>::is_always_lock_free, "used from a signal handler");
    std::signal(SIGINT, on_interrupt);
    std::signal(SIGTERM, on_interrupt);
#ifdef SIGBREAK
    std::signal(SIGBREAK, on_interrupt); // Ctrl+Break on Windows
#endif
    return g_interrupted;
}

} // namespace cli
