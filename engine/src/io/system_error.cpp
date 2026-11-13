#include "io/system_error.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace digzip::io {

std::string system_message(int error)
{
#ifdef _WIN32
    wchar_t* text = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                            FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, static_cast<DWORD>(error), 0, reinterpret_cast<LPWSTR>(&text), 0,
                                        nullptr);
    if (length == 0 || text == nullptr) {
        return "system error " + std::to_string(error);
    }
    std::wstring wide(text, length);
    LocalFree(text);
    while (!wide.empty() && (wide.back() == L'\n' || wide.back() == L'\r' || wide.back() == L' ' || wide.back() == L'.')) {
        wide.pop_back();
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                         nullptr);
    std::string utf8(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), size, nullptr, nullptr);
    return utf8;
#else
    return std::system_category().message(error);
#endif
}

std::string error_message(const std::error_code& ec)
{
    if (ec.category() == std::system_category()) {
        return system_message(ec.value());
    }
    return ec.message();
}

#ifdef _WIN32
void throw_last_error(ErrorCode code, const std::string& what)
{
    throw_system_error(code, what, static_cast<int>(GetLastError()));
}
#endif

} // namespace digzip::io
