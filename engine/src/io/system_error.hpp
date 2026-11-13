#pragma once

#include <digzip/error.hpp>

#include <string>
#include <system_error>

namespace digzip::io {

// Operating system error text, in UTF-8 (Windows reports it in the local code page otherwise).
std::string system_message(int error);

// Same, for error codes coming from std::filesystem.
std::string error_message(const std::error_code& ec);

[[noreturn]] inline void throw_system_error(ErrorCode code, const std::string& what, int error)
{
    throw Error(code, what + ": " + system_message(error));
}

#ifdef _WIN32
[[noreturn]] void throw_last_error(ErrorCode code, const std::string& what);
#endif

} // namespace digzip::io
