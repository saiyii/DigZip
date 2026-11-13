#pragma once

#include <digzip/error.hpp>

#include <string>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace digzip::io {

[[noreturn]] inline void throw_system_error(ErrorCode code, const std::string& what, int error)
{
    throw Error(code, what + ": " + std::system_category().message(error));
}

#ifdef _WIN32
[[noreturn]] inline void throw_last_error(ErrorCode code, const std::string& what)
{
    throw_system_error(code, what, static_cast<int>(GetLastError()));
}
#endif

} // namespace digzip::io
