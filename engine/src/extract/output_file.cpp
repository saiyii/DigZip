#include "extract/output_file.hpp"
#include "io/system_error.hpp"

#include <digzip/error.hpp>

#include <algorithm>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace digzip::extraction {

namespace fs = std::filesystem;

#ifdef _WIN32

namespace {

// FILETIME counts 100 ns intervals since 1601-01-01.
bool to_filetime(const Timestamp& t, FILETIME& out)
{
    constexpr std::int64_t kEpochOffset = 11'644'473'600;
    if (t.seconds < -kEpochOffset || t.seconds > (INT64_MAX / 10'000'000) - kEpochOffset) {
        return false;
    }
    const auto ticks = static_cast<std::uint64_t>((t.seconds + kEpochOffset) * 10'000'000 + t.nanoseconds / 100);
    out.dwLowDateTime = static_cast<DWORD>(ticks);
    out.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
    return true;
}

} // namespace

OutputFile::OutputFile(const fs::path& path, std::optional<std::uint32_t>)
{
    handle_ = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
            throw Error(ErrorCode::AlreadyExists, "the file already exists");
        }
        io::throw_system_error(ErrorCode::Io, "cannot create the file", static_cast<int>(error));
    }
}

OutputFile::~OutputFile()
{
    if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
    }
}

void OutputFile::write(const std::uint8_t* data, std::size_t size)
{
    while (size > 0) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(size, 1u << 30));
        DWORD written = 0;
        if (!WriteFile(handle_, data, chunk, &written, nullptr)) {
            io::throw_last_error(ErrorCode::Io, "cannot write the file");
        }
        data += written;
        size -= written;
    }
}

void OutputFile::set_mtime(const Timestamp& mtime)
{
    FILETIME time{};
    if (to_filetime(mtime, time)) {
        SetFileTime(handle_, nullptr, nullptr, &time);
    }
}

void OutputFile::close()
{
    const BOOL ok = CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
    if (!ok) {
        io::throw_last_error(ErrorCode::Io, "cannot close the file");
    }
}

bool set_directory_mtime(const fs::path& path, const Timestamp& mtime)
{
    FILETIME time{};
    if (!to_filetime(mtime, time)) {
        return false;
    }
    HANDLE dir = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (dir == INVALID_HANDLE_VALUE) {
        return false;
    }
    const BOOL ok = SetFileTime(dir, nullptr, nullptr, &time);
    CloseHandle(dir);
    return ok != FALSE;
}

fs::path long_path(const fs::path& path)
{
    const std::wstring absolute = fs::absolute(path).lexically_normal().wstring();
    if (absolute.rfind(LR"(\\?\)", 0) == 0) {
        return absolute;
    }
    if (absolute.rfind(LR"(\\)", 0) == 0) {
        return LR"(\\?\UNC\)" + absolute.substr(2);
    }
    return LR"(\\?\)" + absolute;
}

#else

namespace {

timespec to_timespec(const Timestamp& t)
{
    timespec out{};
    out.tv_sec = static_cast<time_t>(t.seconds);
    out.tv_nsec = static_cast<long>(t.nanoseconds);
    return out;
}

} // namespace

OutputFile::OutputFile(const fs::path& path, std::optional<std::uint32_t> mode)
{
    const auto permissions = static_cast<mode_t>(mode ? (*mode & 0777) : 0666);
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, permissions);
    if (fd_ < 0) {
        if (errno == EEXIST) {
            throw Error(ErrorCode::AlreadyExists, "the file already exists");
        }
        io::throw_system_error(ErrorCode::Io, "cannot create the file", errno);
    }
}

OutputFile::~OutputFile()
{
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

void OutputFile::write(const std::uint8_t* data, std::size_t size)
{
    while (size > 0) {
        const ssize_t written = ::write(fd_, data, size);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            io::throw_system_error(ErrorCode::Io, "cannot write the file", errno);
        }
        data += written;
        size -= static_cast<std::size_t>(written);
    }
}

void OutputFile::set_mtime(const Timestamp& mtime)
{
    const timespec times[2] = {{0, UTIME_OMIT}, to_timespec(mtime)};
    ::futimens(fd_, times);
}

void OutputFile::close()
{
    const int result = ::close(fd_);
    fd_ = -1;
    if (result != 0) {
        io::throw_system_error(ErrorCode::Io, "cannot close the file", errno);
    }
}

bool set_directory_mtime(const fs::path& path, const Timestamp& mtime)
{
    const timespec times[2] = {{0, UTIME_OMIT}, to_timespec(mtime)};
    return ::utimensat(AT_FDCWD, path.c_str(), times, AT_SYMLINK_NOFOLLOW) == 0;
}

fs::path long_path(const fs::path& path)
{
    return fs::absolute(path).lexically_normal();
}

#endif

} // namespace digzip::extraction
