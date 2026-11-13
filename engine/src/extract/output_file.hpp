#pragma once

#include <digzip/entry.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>

namespace digzip::extraction {

// A new file opened for writing. The destructor closes it without reporting errors.
class OutputFile {
public:
    // Creates the file; throws Error(AlreadyExists) if the path exists, Error(Io) otherwise.
    // `mode` (POSIX permission bits, setuid/setgid/sticky dropped) is filtered by the umask.
    OutputFile(const std::filesystem::path& path, std::optional<std::uint32_t> mode);
    OutputFile(const OutputFile&) = delete;
    OutputFile& operator=(const OutputFile&) = delete;
    ~OutputFile();

    void write(const std::uint8_t* data, std::size_t size);
    void set_mtime(const Timestamp& mtime);
    void close();

private:
#ifdef _WIN32
    void* handle_;
#else
    int fd_;
#endif
};

// Best effort: returns false if the time could not be set.
bool set_directory_mtime(const std::filesystem::path& path, const Timestamp& mtime);

// Absolute, normalized form of `path`. On Windows it carries the \\?\ prefix so that paths
// longer than MAX_PATH work.
std::filesystem::path long_path(const std::filesystem::path& path);

} // namespace digzip::extraction
