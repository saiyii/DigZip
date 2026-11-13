#include "support/fixtures.hpp"

#include <digzip/archive.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace digzip;
using fixtures::TempDir;
namespace fs = std::filesystem;

namespace {

// Extracts into <sandbox>/out so that anything escaping lands in <sandbox> and is detected.
struct Sandbox {
    TempDir dir;
    fs::path out = dir.path() / "out";

    Report run(const std::string& archive, const ExtractOptions& options = {})
    {
        return extract(fixtures::path(archive), out, options);
    }

    std::vector<std::string> outside() const
    {
        std::vector<std::string> escaped;
        for (const auto& entry : fs::directory_iterator(dir.path())) {
            if (entry.path().filename() != "out") {
                escaped.push_back(fixtures::utf8(entry.path().filename()));
            }
        }
        return escaped;
    }
};

} // namespace

TEST_CASE("zip-slip style paths are refused and the rest is extracted")
{
    const std::string name = GENERATE("evil-traversal.tar", "evil-absolute.tar", "evil-drive.tar", "evil-backslash.tar");
    CAPTURE(name);
    Sandbox sandbox;
    const Report report = sandbox.run(name);

    CHECK_FALSE(report.ok());
    CHECK(fixtures::has_issue(report, ErrorCode::Security));
    CHECK(report.files == 1);
    CHECK(fixtures::tree(sandbox.out) == std::vector<std::string>{"safe.txt"});
    CHECK(sandbox.outside().empty());
    CHECK_FALSE(fs::exists("/tmp/digzip-evil.txt"));
    CHECK_FALSE(fs::exists("C:/digzip-evil.txt"));
}

TEST_CASE("test() reports hostile paths without writing anything")
{
    const Report report = test(fixtures::path("evil-traversal.tar"));
    CHECK_FALSE(report.ok());
    CHECK(fixtures::has_issue(report, ErrorCode::Security, "../evil.txt"));
    CHECK(report.files == 1);
}

TEST_CASE("symbolic links pointing outside are refused")
{
    Sandbox sandbox;
    const Report report = sandbox.run("evil-symlink.tar");

    CHECK(fixtures::has_issue(report, ErrorCode::Security, "up"));
    CHECK(fixtures::has_issue(report, ErrorCode::Security, "abs"));
    CHECK(fixtures::has_issue(report, ErrorCode::Security, "sneaky"));
    CHECK(fixtures::tree(sandbox.out) == std::vector<std::string>{"safe.txt"});
    CHECK(sandbox.outside().empty());
}

TEST_CASE("files are never written through a symbolic link from the archive")
{
    Sandbox sandbox;
    const Report report = sandbox.run("evil-write-through.tar");

    // "inside" became a real directory; the link, created last, finds it taken.
    CHECK(fs::is_directory(fs::symlink_status(sandbox.out / "inside")));
    CHECK(fixtures::read_file(sandbox.out / "inside/file.txt") == fixtures::bytes("through\n"));
    CHECK(fixtures::has_issue(report, ErrorCode::AlreadyExists, "inside"));
    CHECK_FALSE(fs::exists(sandbox.out / "real"));
}

TEST_CASE("hard links may only target files extracted from the same archive")
{
    Sandbox sandbox;
    const Report report = sandbox.run("evil-hardlink.tar");

    CHECK(fixtures::has_issue(report, ErrorCode::Security, "h-abs"));
    CHECK(fixtures::has_issue(report, ErrorCode::Security, "h-up"));
    CHECK(fixtures::has_issue(report, ErrorCode::Security, "h-missing"));
    CHECK(fixtures::tree(sandbox.out) == std::vector<std::string>{"safe.txt"});
}

TEST_CASE("devices and FIFOs are skipped with a warning")
{
    Sandbox sandbox;
    const Report report = sandbox.run("special.tar");

    CHECK(report.ok());
    CHECK(report.skipped == 3);
    CHECK(fixtures::has_issue(report, ErrorCode::Security, "fifo"));
    CHECK(fixtures::tree(sandbox.out) == std::vector<std::string>{"safe.txt"});
}

TEST_CASE("existing files are left untouched")
{
    Sandbox sandbox;
    fs::create_directories(sandbox.out);
    fixtures::write_file(sandbox.out / "hello.txt", "mine");

    const Report report = sandbox.run("basic.tar");
    CHECK(report.ok());
    CHECK(fixtures::has_issue(report, ErrorCode::AlreadyExists, "hello.txt"));
    CHECK(fixtures::read_file(sandbox.out / "hello.txt") == fixtures::bytes("mine"));
}

TEST_CASE("a file in the way of a directory is an error for that entry only")
{
    Sandbox sandbox;
    fs::create_directories(sandbox.out);
    fixtures::write_file(sandbox.out / "docs", "not a directory");

    const Report report = sandbox.run("basic.tar");
    CHECK(fixtures::has_issue(report, ErrorCode::AlreadyExists, "docs/readme.md"));
    CHECK(fixtures::read_file(sandbox.out / "hello.txt") == fixtures::bytes("Hello, DigZip!\n"));
}

TEST_CASE("names invalid on the host are fixed or refused")
{
    Sandbox sandbox;
    const Report report = sandbox.run("windows-names.tar");
    CHECK(report.ok());
    CHECK(report.files == 7);
#ifdef _WIN32
    CHECK(fixtures::has_issue(report, ErrorCode::InvalidName, "con.txt"));
    CHECK(fs::exists(sandbox.out / "con_.txt"));
    CHECK(fs::exists(sandbox.out / "ab_c.txt"));
    CHECK(fs::exists(sandbox.out / "trailing__"));
#else
    CHECK(fs::exists(sandbox.out / "ab:c.txt"));
#endif

    const Report long_name = sandbox.run("long-component.tar");
    CHECK(fixtures::has_issue(long_name, ErrorCode::InvalidName));
}

#ifndef _WIN32
#include <sys/stat.h>

TEST_CASE("setuid and setgid bits are never restored")
{
    Sandbox sandbox;
    REQUIRE(sandbox.run("setuid.tar").ok());
    struct stat info {};
    REQUIRE(::stat((sandbox.out / "suid.sh").c_str(), &info) == 0);
    CHECK((info.st_mode & (S_ISUID | S_ISGID | S_ISVTX)) == 0);
    CHECK((info.st_mode & S_IXUSR) != 0);
}

TEST_CASE("an existing symbolic link in the destination is never followed")
{
    Sandbox sandbox;
    fs::create_directories(sandbox.out);
    fs::create_directories(sandbox.dir.path() / "elsewhere");
    fs::create_directory_symlink(sandbox.dir.path() / "elsewhere", sandbox.out / "docs");
    if (!fs::is_symlink(sandbox.out / "docs")) {
        SKIP("this system emulates directory links with copies (MSYS2, Cygwin)");
    }

    const Report report = sandbox.run("basic.tar");
    CHECK(fixtures::has_issue(report, ErrorCode::Security, "docs/readme.md"));
    CHECK(fs::is_empty(sandbox.dir.path() / "elsewhere"));
}
#endif
