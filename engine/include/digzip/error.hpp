#pragma once

#include <digzip/export.hpp>

#include <stdexcept>
#include <string>

// Exception types must keep default visibility so they can be caught across a shared library boundary.
#if defined(_MSC_VER)
#define DIGZIP_EXCEPTION
#else
#define DIGZIP_EXCEPTION __attribute__((visibility("default")))
#endif

namespace digzip {

enum class ErrorCode {
    Io,              // the archive cannot be read or the output cannot be written
    Corrupt,         // malformed or truncated data
    Unsupported,     // valid data using a format, method or feature DigZip does not handle
    Checksum,        // CRC or integrity check mismatch
    Security,        // entry refused: path traversal, unsafe link, special file
    LimitExceeded,   // an anti-bomb limit was reached
    InvalidName,     // a file name had to be changed or cannot be represented on this system
    AlreadyExists,   // the output path already exists
    InvalidArgument,  // bad option or destination
    Cancelled,        // stopped at the caller's request
    PasswordRequired, // the data is encrypted and no password was given
    WrongPassword,    // none of the given passwords decrypts the data
};

DIGZIP_EXPORT const char* to_string(ErrorCode code) noexcept;

class DIGZIP_EXCEPTION Error : public std::runtime_error {
public:
    Error(ErrorCode code, const std::string& message) : std::runtime_error(message), code_(code) {}

    ErrorCode code() const noexcept { return code_; }

private:
    ErrorCode code_;
};

} // namespace digzip
