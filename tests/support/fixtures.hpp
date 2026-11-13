#pragma once

#include <digzip/archive.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

namespace fixtures {

namespace fs = std::filesystem;

inline fs::path path(const std::string& name)
{
    return fs::path(DIGZIP_FIXTURES_DIR) / name;
}

// Same generator as make_fixtures.py.
inline std::vector<std::uint8_t> lcg_bytes(std::size_t count, std::uint32_t seed = 12345)
{
    std::vector<std::uint8_t> out;
    out.reserve(count);
    std::uint32_t x = seed;
    for (std::size_t i = 0; i < count; ++i) {
        x = (x * 1103515245u + 12345u) & 0x7FFFFFFFu;
        out.push_back(static_cast<std::uint8_t>((x >> 16) & 0xFF));
    }
    return out;
}

// Same as x86_like() in make_fixtures.py: random bytes with many CALL/JMP opcodes.
inline std::vector<std::uint8_t> x86_like(std::size_t count)
{
    auto data = lcg_bytes(count, 99);
    for (std::size_t i = 0; i + 5 < count; i += 23) {
        data[i] = i % 2 ? 0xE8 : 0xE9;
    }
    return data;
}

inline std::vector<std::uint8_t> bytes(const std::string& s)
{
    return {s.begin(), s.end()};
}

inline std::vector<std::uint8_t> read_file(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

inline void write_file(const fs::path& file, const std::string& content)
{
    std::ofstream out(file, std::ios::binary);
    out << content;
}

// Expected content of the "basic" fixture tree.
inline std::vector<std::pair<std::string, std::vector<std::uint8_t>>> basic_files()
{
    std::string readme;
    for (int i = 0; i < 50; ++i) {
        readme += "# DigZip\n";
    }
    return {
        {"hello.txt", bytes("Hello, DigZip!\n")},
        {"docs/readme.md", bytes(readme)},
        {"docs/empty.txt", {}},
        {"data/random.bin", lcg_bytes(20000)},
        {"unicode/caf\xC3\xA9 \xE2\x98\x95.txt", bytes("unicode\n")},
        {"bin/run.sh", bytes("#!/bin/sh\necho ok\n")},
    };
}

// Fresh directory under the system temp directory, removed on destruction.
class TempDir {
public:
    TempDir()
    {
        std::random_device random;
        path_ = fs::temp_directory_path() / ("digzip-test-" + std::to_string(random()) + std::to_string(random()));
        fs::create_directories(path_);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir()
    {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    const fs::path& path() const { return path_; }
    fs::path operator/(const std::string& relative) const { return path_ / fs::u8path(relative); }

private:
    fs::path path_;
};

inline bool has_issue(const digzip::Report& report, digzip::ErrorCode code, const std::string& entry_path = {})
{
    return std::any_of(report.issues.begin(), report.issues.end(), [&](const digzip::Issue& issue) {
        return issue.code == code && (entry_path.empty() || issue.path == entry_path);
    });
}

// Every file below `root`, relative and '/' separated, including temporary leftovers.
inline std::vector<std::string> tree(const fs::path& root)
{
    std::vector<std::string> out;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        out.push_back(fs::relative(entry.path(), root).generic_u8string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

inline bool has_temp_leftovers(const fs::path& root)
{
    const auto files = tree(root);
    return std::any_of(files.begin(), files.end(),
                       [](const std::string& f) { return f.find(".digzip-") != std::string::npos; });
}

} // namespace fixtures
