#include "io/stream.hpp"
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

namespace digzip::io {

namespace {

[[noreturn]] void throw_past_end()
{
    throw Error(ErrorCode::Corrupt, "unexpected end of archive");
}

#ifdef _WIN32

class FileSource final : public Source {
public:
    explicit FileSource(const std::filesystem::path& path)
    {
        handle_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw_last_error(ErrorCode::Io, "cannot open " + path.u8string());
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(handle_, &size)) {
            const DWORD error = GetLastError();
            CloseHandle(handle_);
            throw_system_error(ErrorCode::Io, "cannot read the size of " + path.u8string(), static_cast<int>(error));
        }
        size_ = static_cast<std::uint64_t>(size.QuadPart);
    }

    ~FileSource() override { CloseHandle(handle_); }

    std::uint64_t size() const override { return size_; }

    void read_at(std::uint64_t offset, std::uint8_t* data, std::size_t size) const override
    {
        if (offset > size_ || size > size_ - offset) {
            throw_past_end();
        }
        while (size > 0) {
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(size, 1u << 30));
            OVERLAPPED position{};
            position.Offset = static_cast<DWORD>(offset);
            position.OffsetHigh = static_cast<DWORD>(offset >> 32);
            DWORD got = 0;
            if (!ReadFile(handle_, data, chunk, &got, &position)) {
                throw_last_error(ErrorCode::Io, "cannot read the archive");
            }
            if (got == 0) {
                throw_past_end();
            }
            data += got;
            size -= got;
            offset += got;
        }
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::uint64_t size_ = 0;
};

#else

class FileSource final : public Source {
public:
    explicit FileSource(const std::filesystem::path& path)
    {
        fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd_ < 0) {
            throw_system_error(ErrorCode::Io, "cannot open " + path.u8string(), errno);
        }
        struct stat info {};
        if (::fstat(fd_, &info) != 0) {
            const int error = errno;
            ::close(fd_);
            throw_system_error(ErrorCode::Io, "cannot read the size of " + path.u8string(), error);
        }
        if (!S_ISREG(info.st_mode)) {
            ::close(fd_);
            throw Error(ErrorCode::Io, path.u8string() + " is not a regular file");
        }
        size_ = static_cast<std::uint64_t>(info.st_size);
    }

    ~FileSource() override { ::close(fd_); }

    std::uint64_t size() const override { return size_; }

    void read_at(std::uint64_t offset, std::uint8_t* data, std::size_t size) const override
    {
        if (offset > size_ || size > size_ - offset) {
            throw_past_end();
        }
        while (size > 0) {
            const ssize_t got = ::pread(fd_, data, size, static_cast<off_t>(offset));
            if (got < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw_system_error(ErrorCode::Io, "cannot read the archive", errno);
            }
            if (got == 0) {
                throw_past_end();
            }
            data += got;
            size -= static_cast<std::size_t>(got);
            offset += static_cast<std::uint64_t>(got);
        }
    }

private:
    int fd_ = -1;
    std::uint64_t size_ = 0;
};

#endif

} // namespace

SourcePtr open_file(const std::filesystem::path& path)
{
    return std::make_shared<FileSource>(path);
}

} // namespace digzip::io
