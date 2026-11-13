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

} // namespace

TEST_CASE("formats are detected from content")
{
    CHECK(to_string(detect(fixtures::path("basic.tar"))) == "tar");
    CHECK(to_string(detect(fixtures::path("basic.tar.gz"))) == "tar.gz");
    CHECK(to_string(detect(fixtures::path("basic.tar.bz2"))) == "tar.bz2");
    CHECK(to_string(detect(fixtures::path("basic.tar.xz"))) == "tar.xz");
    CHECK(to_string(detect(fixtures::path("basic.tar.zst"))) == "tar.zst");
    CHECK(to_string(detect(fixtures::path("hello.txt.gz"))) == "gz");
    CHECK(to_string(detect(fixtures::path("hello.txt.zst"))) == "zst");
    CHECK(to_string(detect(fixtures::path("garbage.bin"))) == "unknown");
}

TEST_CASE("tar archives are listed with types, sizes, modes and times")
{
    const std::string name = GENERATE("basic.tar", "basic.tar.gz", "basic.tar.bz2", "basic.tar.xz", "basic.tar.zst");
    CAPTURE(name);
    const auto entries = by_path(list(fixtures::path(name)));

    REQUIRE(entries.size() == 9);
    CHECK(entries.at("hello.txt").type == EntryType::File);
    CHECK(entries.at("hello.txt").size == 15u);
    CHECK(entries.at("hello.txt").mtime->seconds == 1'700'000'000);
    CHECK(entries.at("bin/run.sh").mode == 0755u);
    CHECK(entries.at("data/empty_dir").type == EntryType::Directory);
    CHECK(entries.at("links/hello-link").type == EntryType::Symlink);
    CHECK(entries.at("links/hello-link").link_target == "../hello.txt");
    CHECK(entries.at("links/hello-hard").type == EntryType::Hardlink);
    CHECK(entries.at("links/hello-hard").link_target == "hello.txt");
    CHECK(entries.count("unicode/caf\xC3\xA9 \xE2\x98\x95.txt") == 1);
}

TEST_CASE("tar archives extract with exact content")
{
    const std::string name = GENERATE("basic.tar", "basic.tar.gz", "basic.tar.bz2", "basic.tar.xz", "basic.tar.zst");
    CAPTURE(name);
    TempDir out;
    const Report report = extract(fixtures::path(name), out.path());

    // 6 files, 1 hard link, 1 symbolic link (a copy on Windows without the symlink privilege).
    CHECK(report.files + report.links == 8);
    CHECK(report.directories == 1);
    CHECK(fs::is_directory(out / "data/empty_dir"));
    CHECK(fixtures::read_file(out / "links/hello-link") == fixtures::bytes("Hello, DigZip!\n"));
    for (const auto& [path, content] : fixtures::basic_files()) {
        CAPTURE(path);
        CHECK(fixtures::read_file(out / path) == content);
    }
    CHECK(fixtures::read_file(out / "links/hello-hard") == fixtures::bytes("Hello, DigZip!\n"));
    CHECK(fixtures::has_temp_leftovers(out.path()) == false);

    // Stored time is 2023-11-14: restored files must be far older than the extraction itself.
    const auto mtime = fs::last_write_time(out / "hello.txt");
    const bool same_time = mtime == fs::last_write_time(out / "docs/readme.md");
    const bool restored = fs::file_time_type::clock::now() - mtime > std::chrono::hours(24 * 365);
    CHECK(same_time);
    CHECK(restored);
}

TEST_CASE("tar extraction is verified by test()")
{
    const std::string name = GENERATE("basic.tar", "basic.tar.gz", "basic.tar.bz2", "basic.tar.xz", "basic.tar.zst");
    CAPTURE(name);
    const Report report = test(fixtures::path(name));
    CHECK(report.ok());
    CHECK(report.files == 6);
    CHECK(report.bytes == 15 + 450 + 20000 + 8 + 18);
}

TEST_CASE("GNU long names and ustar prefixes are joined")
{
    const auto gnu = list(fixtures::path("gnu-longname.tar"));
    REQUIRE(gnu.entries.size() == 1);
    CHECK(gnu.entries[0].path == "long/" + std::string(120, 'n') + ".txt");

    const auto ustar = list(fixtures::path("ustar-prefix.tar"));
    REQUIRE(ustar.entries.size() == 1);
    CHECK(ustar.entries[0].path == std::string(90, 'p') + "/" + std::string(60, 'q') + ".txt");
}

TEST_CASE("single compressed files extract under their own name")
{
    const std::string name = GENERATE("hello.txt.gz", "hello.txt.bz2", "hello.txt.xz", "hello.txt.zst");
    CAPTURE(name);
    TempDir out;
    const Report report = extract(fixtures::path(name), out.path());
    CHECK(report.ok());
    CHECK(fixtures::read_file(out / "hello.txt") == fixtures::bytes("Hello, DigZip!\n"));
}

TEST_CASE("a gzip file without a stored name uses the archive name")
{
    const auto info = list(fixtures::path("noname.gz"));
    REQUIRE(info.entries.size() == 1);
    CHECK(info.entries[0].path == "noname");
}

TEST_CASE("concatenated streams are decoded entirely")
{
    const std::string name = GENERATE("multi-member.gz", "concat.bz2", "concat.xz", "concat.zst");
    CAPTURE(name);
    TempDir out;
    const Report report = extract(fixtures::path(name), out.path());
    REQUIRE(report.ok());
    REQUIRE(report.files == 1);
    const auto files = fixtures::tree(out.path());
    REQUIRE(files.size() == 1);
    CHECK(fixtures::read_file(out / files[0]) == fixtures::bytes("part1\npart2\n"));
}

TEST_CASE("unrecognized data is reported, not extracted")
{
    TempDir out;
    const Report report = extract(fixtures::path("garbage.bin"), out.path());
    CHECK_FALSE(report.ok());
    CHECK(fixtures::has_issue(report, ErrorCode::Unsupported));
    CHECK(fixtures::tree(out.path()).empty());
}

TEST_CASE("a missing archive is an I/O error")
{
    CHECK_THROWS_AS(list(fixtures::path("does-not-exist.tar")), Error);
    const Report report = test(fixtures::path("does-not-exist.tar"));
    CHECK(fixtures::has_issue(report, ErrorCode::Io));
}
