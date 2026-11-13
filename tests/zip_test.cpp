#include "support/fixtures.hpp"

#include <digzip/archive.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <map>

using namespace digzip;
using fixtures::TempDir;
namespace fs = std::filesystem;

namespace {

std::map<std::string, Entry> by_path(const ArchiveInfo& info)
{
    std::map<std::string, Entry> out;
    for (const Entry& e : info.entries) {
        out[e.path] = e;
    }
    return out;
}

void check_files(const fs::path& root, const std::vector<std::string>& names)
{
    const auto expected = fixtures::basic_files();
    for (const auto& [path, content] : expected) {
        if (std::find(names.begin(), names.end(), path) != names.end()) {
            CAPTURE(path);
            CHECK(fixtures::read_file(root / fs::u8path(path)) == content);
        }
    }
}

const std::vector<std::string> kFiles = {"hello.txt", "docs/readme.md", "docs/empty.txt", "data/random.bin",
                                         "unicode/caf\xC3\xA9 \xE2\x98\x95.txt"};

} // namespace

TEST_CASE("zip entries are listed with methods, sizes, modes and times")
{
    const auto [name, method] = GENERATE(table<std::string, std::string>({
        {"basic-stored.zip", "stored"},
        {"basic-deflate.zip", "deflate"},
        {"basic-bzip2.zip", "bzip2"},
        {"basic-lzma.zip", "lzma"},
        {"basic-zstd.zip", "zstd"},
    }));
    CAPTURE(name);
    const ArchiveInfo info = list(fixtures::path(name));
    CHECK(to_string(info.format) == "zip");
    const auto entries = by_path(info);
    REQUIRE(entries.size() == 8);
    CHECK(entries.at("hello.txt").method == method);
    CHECK(entries.at("hello.txt").size == 15u);
    CHECK(entries.at("hello.txt").crc32 == 0xDD4C15FBu);
    CHECK(entries.at("hello.txt").mtime->seconds == 1'700'000'000);
    CHECK(entries.at("bin/run.sh").mode == 0755u);
    CHECK(entries.at("data/empty_dir").type == EntryType::Directory);
    CHECK(entries.at("links/hello-link").type == EntryType::Symlink);
    CHECK(entries.at("links/hello-link").link_target == "../hello.txt");
}

TEST_CASE("zip archives extract with exact content for every method")
{
    const std::string name =
        GENERATE("basic-stored.zip", "basic-deflate.zip", "basic-bzip2.zip", "basic-lzma.zip", "basic-zstd.zip");
    CAPTURE(name);
    TempDir out;
    const Report report = extract(fixtures::path(name), out.path());
    CHECK(report.ok());
    CHECK(report.files + report.links == 7);
    CHECK(fs::is_directory(out / "data/empty_dir"));
    check_files(out.path(), kFiles);
    CHECK(fixtures::read_file(out / "bin/run.sh") == fixtures::bytes("#!/bin/sh\necho ok\n"));
    CHECK(fixtures::read_file(out / "links/hello-link") == fixtures::bytes("Hello, DigZip!\n"));
    CHECK(test(fixtures::path(name)).ok());
}

TEST_CASE("Deflate64 entries made by 7-Zip decode exactly")
{
    TempDir out;
    REQUIRE(extract(fixtures::path("basic-deflate64.zip"), out.path()).ok());
    check_files(out.path(), kFiles);

    // 50 KB repeated three times: matches 50,000 bytes back, beyond the 32 KiB deflate window.
    REQUIRE(extract(fixtures::path("big-deflate64.zip"), out.path()).ok());
    auto expected = fixtures::lcg_bytes(50'000);
    const auto block = expected;
    expected.insert(expected.end(), block.begin(), block.end());
    expected.insert(expected.end(), block.begin(), block.end());
    expected.resize(expected.size() + 100'000, 0);
    CHECK(fixtures::read_file(out / "big.bin") == expected);
    CHECK(list(fixtures::path("big-deflate64.zip")).entries[0].method == "deflate64");
}

TEST_CASE("zip variants: xz method, data descriptors, Zip64, prepended data, comments, empty")
{
    TempDir out;
    REQUIRE(extract(fixtures::path("xz-method.zip"), out / "xz").ok());
    CHECK(fixtures::read_file(out / "xz/hello.txt") == fixtures::bytes("Hello, DigZip!\n"));

    REQUIRE(extract(fixtures::path("data-descriptor.zip"), out / "dd").ok());
    check_files(out / "dd", kFiles);

    for (const char* name : {"zip64-entry.zip", "zip64-eocd.zip", "comment.zip"}) {
        CAPTURE(name);
        const Report report = extract(fixtures::path(name), out / name);
        CHECK(report.ok());
        CHECK(fixtures::read_file(out / name / "hello.txt") == fixtures::bytes("Hello, DigZip!\n"));
    }

    CHECK(to_string(detect(fixtures::path("sfx.zip"))) == "zip");
    REQUIRE(extract(fixtures::path("sfx.zip"), out / "sfx").ok());
    check_files(out / "sfx", kFiles);

    const Report empty = extract(fixtures::path("empty.zip"), out / "empty");
    CHECK(empty.ok());
    CHECK(empty.files == 0);
}

TEST_CASE("zip names: CP437, Info-ZIP Unicode path, UTF-8 flag")
{
    const auto entries = by_path(list(fixtures::path("names.zip")));
    CHECK(entries.count("caf\xC3\xA9-\xC3\xBC.txt") == 1);
    CHECK(entries.count("\xC3\xA9t\xC3\xA9 \xE2\x98\x95.txt") == 1);
    CHECK(entries.count("utf8-\xC3\xA9.txt") == 1);
}

TEST_CASE("hostile zip entries are refused, the others extracted")
{
    const std::string name = GENERATE("evil-traversal.zip", "evil-absolute.zip", "evil-symlink.zip");
    CAPTURE(name);
    TempDir dir;
    const Report report = extract(fixtures::path(name), dir / "out");
    CHECK(fixtures::has_issue(report, ErrorCode::Security));
    CHECK(fixtures::tree(dir.path()) == std::vector<std::string>{"out", "out/safe.txt"});
}

TEST_CASE("overlapping entries (zip bomb technique) are refused")
{
    TempDir out;
    const Report report = extract(fixtures::path("evil-overlap.zip"), out.path());
    CHECK(fixtures::has_issue(report, ErrorCode::Security));
    CHECK(fixtures::tree(out.path()).empty());
}

TEST_CASE("broken entries fail alone; the rest of the zip is extracted")
{
    const auto [name, bad, code] = GENERATE(table<std::string, std::string, ErrorCode>({
        {"crc-mismatch.zip", "bad.txt", ErrorCode::Checksum},
        {"size-lie.zip", "lie.txt", ErrorCode::Corrupt},
        {"unknown-method.zip", "odd.bin", ErrorCode::Unsupported},
    }));
    CAPTURE(name);
    TempDir out;
    const Report report = extract(fixtures::path(name), out.path());
    CHECK(fixtures::has_issue(report, code, bad));
    CHECK(fixtures::tree(out.path()) == std::vector<std::string>{"good.txt"});
}

TEST_CASE("corrupt compressed data is detected and leaves no partial file")
{
    TempDir out;
    const Report report = extract(fixtures::path("corrupt-data.zip"), out.path());
    CHECK_FALSE(report.ok());
    CHECK_FALSE(fs::exists(out / "data/random.bin"));
    CHECK_FALSE(fixtures::has_temp_leftovers(out.path()));
    CHECK(fixtures::read_file(out / "hello.txt") == fixtures::bytes("Hello, DigZip!\n"));
}

TEST_CASE("truncated, encrypted and oversized zips are reported")
{
    TempDir out;
    CHECK(fixtures::has_issue(extract(fixtures::path("truncated.zip"), out / "t"), ErrorCode::Corrupt));
    CHECK(fixtures::has_issue(extract(fixtures::path("encrypted-flag.zip"), out / "e"), ErrorCode::Unsupported));

    ExtractOptions strict;
    strict.limits.max_ratio = 100;
    strict.limits.ratio_threshold = 1 << 20;
    const Report bomb = extract(fixtures::path("bomb.zip"), out / "b", strict);
    CHECK(fixtures::has_issue(bomb, ErrorCode::LimitExceeded));
    CHECK_FALSE(fixtures::has_temp_leftovers(out.path()));
}
