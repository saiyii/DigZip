#include "support/fixtures.hpp"

#include <digzip/archive.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <map>

using namespace digzip;
using fixtures::TempDir;
namespace fs = std::filesystem;

namespace {

std::vector<std::pair<std::string, std::vector<std::uint8_t>>> expected_files()
{
    auto files = fixtures::basic_files();
    files.erase(std::remove_if(files.begin(), files.end(), [](const auto& f) { return f.first == "bin/run.sh"; }),
                files.end());
    files.emplace_back("data/x86.bin", fixtures::x86_like(16'000));
    return files;
}

std::map<std::string, Entry> by_path(const ArchiveInfo& info)
{
    std::map<std::string, Entry> out;
    for (const Entry& e : info.entries) {
        out[e.path] = e;
    }
    return out;
}

} // namespace

TEST_CASE("7z archives are listed with sizes, methods and times")
{
    const ArchiveInfo info = list(fixtures::path("basic.7z"));
    CHECK(to_string(info.format) == "7z");
    const auto entries = by_path(info);
    CHECK(entries.at("hello.txt").size == 15u);
    CHECK(entries.at("hello.txt").crc32 == 0xDD4C15FBu);
    CHECK(entries.at("hello.txt").method == "lzma2");
    CHECK(entries.at("hello.txt").mtime->seconds == 1'700'000'000);
    CHECK(entries.at("docs/empty.txt").size == 0u);
    CHECK(entries.at("docs/empty.txt").type == EntryType::File);
    CHECK(entries.at("data/x86.bin").size == 16'000u);
}

TEST_CASE("7z archives extract exactly with every coder chain")
{
    const auto [name, method] = GENERATE(table<std::string, std::string>({
        {"basic.7z", "lzma2"},
        {"lzma.7z", "lzma"},
        {"bzip2.7z", "bzip2"},
        {"deflate.7z", "deflate"},
        {"deflate64.7z", "deflate64"},
        {"copy.7z", "copy"},
        {"bcj.7z", "bcj+lzma2"},
        {"arm64.7z", "arm64+lzma2"},
        {"delta.7z", "delta+lzma2"},
        {"bcj2.7z", "bcj2+lzma+lzma+lzma"},
        {"non-solid.7z", "lzma2"},
        {"plain-header.7z", "lzma2"},
    }));
    CAPTURE(name);
    const auto entries = by_path(list(fixtures::path(name)));
    CHECK(entries.at("data/x86.bin").method.find(method.substr(0, method.find('+'))) != std::string::npos);

    TempDir out;
    const Report report = extract(fixtures::path(name), out.path());
    CHECK(report.ok());
    for (const auto& [path, content] : expected_files()) {
        CAPTURE(path);
        CHECK(fixtures::read_file(out / path) == content);
    }
    CHECK(test(fixtures::path(name)).ok());
}

TEST_CASE("7z files written by DigZip's crafted writer, and empty archives")
{
    TempDir out;
    REQUIRE(extract(fixtures::path("crafted.7z"), out.path()).ok());
    CHECK(fixtures::read_file(out / "a.txt") == fixtures::bytes("first\n"));
    CHECK(fixtures::read_file(out / "b/c.txt") == fixtures::bytes("second\n"));

    const Report empty = extract(fixtures::path("empty.7z"), out / "empty");
    CHECK(empty.ok());
    CHECK(empty.files == 0);
}

TEST_CASE("hostile 7z entries are refused, the others extracted")
{
    const std::string name = GENERATE("evil-traversal.7z", "evil-absolute.7z", "evil-symlink.7z");
    CAPTURE(name);
    TempDir dir;
    const Report report = extract(fixtures::path(name), dir / "out");
    CHECK(fixtures::has_issue(report, ErrorCode::Security));
    CHECK(fixtures::tree(dir.path()) == std::vector<std::string>{"out", "out/safe.txt"});
}

TEST_CASE("damaged 7z archives fail cleanly")
{
    const auto [name, code] = GENERATE(table<std::string, ErrorCode>({
        {"truncated.7z", ErrorCode::Corrupt},
        {"bad-header-crc.7z", ErrorCode::Checksum},
        {"unknown-method.7z", ErrorCode::Unsupported},
    }));
    CAPTURE(name);
    TempDir out;
    const Report report = extract(fixtures::path(name), out.path());
    CHECK(fixtures::has_issue(report, code));
    CHECK(fixtures::tree(out.path()).empty());
}

TEST_CASE("corrupt data in a solid block fails its files without partial output")
{
    TempDir out;
    const Report report = extract(fixtures::path("corrupt-data.7z"), out.path());
    CHECK_FALSE(report.ok());
    CHECK_FALSE(fixtures::has_temp_leftovers(out.path()));
    for (const auto& [path, content] : expected_files()) {
        if (fs::exists(out / path)) {
            CAPTURE(path);
            CHECK(fixtures::read_file(out / path) == content);
        }
    }
}

TEST_CASE("in a non-solid 7z, one damaged file does not stop the others")
{
    TempDir out;
    const Report report = extract(fixtures::path("corrupt-non-solid.7z"), out.path());
    CHECK_FALSE(report.ok());
    std::size_t intact = 0;
    for (const auto& [path, content] : expected_files()) {
        if (fs::exists(out / path)) {
            CHECK(fixtures::read_file(out / path) == content);
            ++intact;
        }
    }
    CHECK(intact >= expected_files().size() - 1);
}

TEST_CASE("a 7z decompression bomb is stopped")
{
    ExtractOptions options;
    options.limits.max_ratio = 100;
    options.limits.ratio_threshold = 1 << 20;
    TempDir out;
    const Report report = extract(fixtures::path("bomb.7z"), out.path(), options);
    CHECK(fixtures::has_issue(report, ErrorCode::LimitExceeded));
    CHECK(fixtures::tree(out.path()).empty());
}

TEST_CASE("PPMd (variant H) 7z archives decode exactly")
{
    const std::string name =
        GENERATE("ppmd.7z", "ppmd-o2.7z", "ppmd-o32.7z", "ppmd-small-memory.7z");
    CAPTURE(name);
    TempDir out;
    const Report report = extract(fixtures::path(name), out.path());
    CHECK(report.ok());
    CHECK(fixtures::read_file(out / "text.txt") == fixtures::word_text(40'000));
    CHECK(list(fixtures::path(name)).entries[0].method == "ppmd");
}

TEST_CASE("PPMd handles incompressible data")
{
    TempDir out;
    REQUIRE(extract(fixtures::path("ppmd-binary.7z"), out.path()).ok());
    CHECK(fixtures::read_file(out / "data/random.bin") == fixtures::lcg_bytes(20000));
    CHECK(fixtures::read_file(out / "hello.txt") == fixtures::bytes("Hello, DigZip!\n"));
}
