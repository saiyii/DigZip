#include "filter.hpp"
#include "support/fixtures.hpp"

#include <digzip/archive.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace digzip;
using fixtures::TempDir;
namespace fs = std::filesystem;

namespace {

std::vector<std::string> files_of(const fs::path& root)
{
    std::vector<std::string> out;
    for (const auto& path : fixtures::tree(root)) {
        if (fs::is_regular_file(root / fs::u8path(path))) {
            out.push_back(path);
        }
    }
    return out;
}

} // namespace

TEST_CASE("glob patterns")
{
    CHECK(glob_match("*", ""));
    CHECK(glob_match("*", "a/b/c"));
    CHECK(glob_match("*.txt", "hello.txt"));
    CHECK(glob_match("*.txt", "docs/empty.txt"));
    CHECK_FALSE(glob_match("*.txt", "hello.txt.bak"));
    CHECK(glob_match("hell?.txt", "hello.txt"));
    CHECK_FALSE(glob_match("hell?.txt", "hell.txt"));
    CHECK(glob_match("a*b*c", "aXXbYYc"));
    CHECK_FALSE(glob_match("a*b*c", "aXXbYY"));
    CHECK(glob_match("**", "anything"));
    CHECK_FALSE(glob_match("Hello.txt", "hello.txt"));
}

TEST_CASE("include and exclude select entries for list, test and extract")
{
    const auto [include, exclude, expected] = GENERATE(table<std::vector<std::string>, std::vector<std::string>,
                                                             std::vector<std::string>>({
        {{"docs"}, {}, {"docs/empty.txt", "docs/readme.md"}},
        {{"*.md"}, {}, {"docs/readme.md"}},
        {{"hell?.txt"}, {}, {"hello.txt"}},
        {{"*.txt"}, {"unicode", "docs"}, {"hello.txt"}},
        {{}, {"data", "docs", "unicode", "links", "bin"}, {"hello.txt"}},
        {{"nothing*"}, {}, {}},
    }));
    CAPTURE(include, exclude);

    ExtractOptions options;
    options.include = include;
    options.exclude = exclude;

    std::vector<std::string> listed;
    for (const Entry& e : list(fixtures::path("basic-deflate.zip"), options).entries) {
        if (e.type == EntryType::File) {
            listed.push_back(e.path);
        }
    }
    std::sort(listed.begin(), listed.end());
    CHECK(listed == expected);

    const Report tested = test(fixtures::path("basic-deflate.zip"), options);
    CHECK(tested.files == expected.size());

    TempDir out;
    REQUIRE(extract(fixtures::path("basic-deflate.zip"), out.path(), options).ok());
    CHECK(files_of(out.path()) == expected);
}

TEST_CASE("strip_components removes leading directories")
{
    ExtractOptions options;
    options.strip_components = 1;
    options.include = {"docs", "data", "unicode"};
    TempDir out;
    const Report report = extract(fixtures::path("basic.tar"), out.path(), options);
    CHECK(report.ok());
    CHECK(files_of(out.path()) ==
          std::vector<std::string>{"caf\xC3\xA9 \xE2\x98\x95.txt", "empty.txt", "random.bin", "readme.md"});
    CHECK(fs::is_directory(out / "empty_dir"));
}

TEST_CASE("strip_components never lets a link escape")
{
    ExtractOptions options;
    options.strip_components = 1;
    TempDir out;
    const Report report = extract(fixtures::path("basic.tar"), out.path(), options);
    // links/hello-link -> ../hello.txt now sits at the root and would point outside.
    CHECK(fixtures::has_issue(report, ErrorCode::Security, "links/hello-link"));
    // links/hello-hard targets hello.txt, which was stripped away.
    CHECK(fixtures::has_issue(report, ErrorCode::AlreadyExists, "links/hello-hard"));
    CHECK_FALSE(fs::exists(out / "hello.txt"));
}

TEST_CASE("overwrite policies")
{
    TempDir out;
    fixtures::write_file(out / "hello.txt", "mine");

    SECTION("replace")
    {
        ExtractOptions options;
        options.overwrite = Overwrite::Replace;
        CHECK(extract(fixtures::path("basic.tar"), out.path(), options).ok());
        CHECK(fixtures::read_file(out / "hello.txt") == fixtures::bytes("Hello, DigZip!\n"));
        CHECK(extract(fixtures::path("basic.tar"), out.path(), options).ok());
        CHECK(fixtures::read_file(out / "links/hello-hard") == fixtures::bytes("Hello, DigZip!\n"));
    }

    SECTION("rename")
    {
        ExtractOptions options;
        options.overwrite = Overwrite::Rename;
        const Report first = extract(fixtures::path("basic.tar"), out.path(), options);
        CHECK(first.ok());
        CHECK(fixtures::has_issue(first, ErrorCode::AlreadyExists, "hello.txt"));
        CHECK(fixtures::read_file(out / "hello.txt") == fixtures::bytes("mine"));
        CHECK(fixtures::read_file(out / "hello (1).txt") == fixtures::bytes("Hello, DigZip!\n"));
        // The hard link follows the renamed file.
        CHECK(fixtures::read_file(out / "links/hello-hard") == fixtures::bytes("Hello, DigZip!\n"));

        CHECK(extract(fixtures::path("basic.tar"), out.path(), options).ok());
        CHECK(fs::exists(out / "hello (2).txt"));
        CHECK(fs::exists(out / "docs/readme (1).md"));
    }

    SECTION("a directory in the way cannot be replaced by a file")
    {
        fs::create_directories(out / "docs/readme.md");
        ExtractOptions options;
        options.overwrite = Overwrite::Replace;
        const Report report = extract(fixtures::path("basic.tar"), out.path(), options);
        CHECK(fixtures::has_issue(report, ErrorCode::AlreadyExists, "docs/readme.md"));
        CHECK(fs::is_directory(out / "docs/readme.md"));
    }
}

TEST_CASE("progress is reported up to the end of the archive")
{
    const std::string name = GENERATE("basic.tar.gz", "basic-deflate.zip", "basic.7z");
    CAPTURE(name);
    std::vector<Progress> calls;
    ExtractOptions options;
    options.on_progress = [&](const Progress& p) { calls.push_back(p); };
    TempDir out;
    const Report report = extract(fixtures::path(name), out.path(), options);
    CHECK(report.ok());
    REQUIRE(calls.size() >= 2);
    for (std::size_t i = 1; i < calls.size(); ++i) {
        CHECK(calls[i].bytes >= calls[i - 1].bytes);
        CHECK(calls[i].entries >= calls[i - 1].entries);
    }
    const Progress& last = calls.back();
    CHECK(last.archive_size == fs::file_size(fixtures::path(name)));
    CHECK(last.archive_read > last.archive_size / 2);
    CHECK(last.archive_read <= last.archive_size);
    CHECK(last.current.empty());
}

TEST_CASE("cancellation stops cleanly")
{
    std::atomic<bool> cancel{false};
    ExtractOptions options;
    options.cancel = &cancel;

    SECTION("before the start")
    {
        cancel = true;
        TempDir out;
        const Report report = extract(fixtures::path("basic.tar"), out.path(), options);
        CHECK(fixtures::has_issue(report, ErrorCode::Cancelled));
        CHECK(fixtures::tree(out.path()).empty());
    }

    SECTION("between entries")
    {
        options.on_progress = [&](const Progress& p) {
            if (p.entries == 4) {
                cancel = true;
            }
        };
        TempDir out;
        const Report report = extract(fixtures::path("basic.tar"), out.path(), options);
        CHECK(fixtures::has_issue(report, ErrorCode::Cancelled));
        CHECK(report.files < 6);
        CHECK_FALSE(fixtures::has_temp_leftovers(out.path()));
    }

    SECTION("in the middle of a large file")
    {
        options.on_progress = [&](const Progress& p) {
            if (p.bytes > 0) {
                cancel = true;
            }
        };
        TempDir out;
        const Report report = extract(fixtures::path("bomb.gz"), out.path(), options);
        CHECK(fixtures::has_issue(report, ErrorCode::Cancelled));
        CHECK(fixtures::tree(out.path()).empty());
    }
}
