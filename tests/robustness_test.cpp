#include "support/fixtures.hpp"

#include <digzip/archive.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace digzip;
using fixtures::TempDir;
namespace fs = std::filesystem;

TEST_CASE("a decompression bomb is stopped by the ratio limit")
{
    const std::string name = GENERATE("bomb.tar.gz", "bomb.gz");
    CAPTURE(name);
    ExtractOptions options;
    options.limits.max_ratio = 100;
    options.limits.ratio_threshold = 1 << 20;
    TempDir out;
    const Report report = extract(fixtures::path(name), out.path(), options);

    CHECK(fixtures::has_issue(report, ErrorCode::LimitExceeded));
    CHECK(fixtures::tree(out.path()).empty());
}

TEST_CASE("the default limits accept a 1000:1 archive under the threshold")
{
    TempDir out;
    const Report report = extract(fixtures::path("bomb.gz"), out.path());
    CHECK(report.ok());
    CHECK(fs::file_size(out / "zeros.bin") == 32u << 20);
}

TEST_CASE("total size, entry count and depth limits")
{
    TempDir out;
    ExtractOptions size_limited;
    size_limited.limits.max_total_size = 1 << 20;
    CHECK(fixtures::has_issue(extract(fixtures::path("bomb.tar.gz"), out / "a", size_limited), ErrorCode::LimitExceeded));

    ExtractOptions few_entries;
    few_entries.limits.max_entries = 10;
    const Report many = extract(fixtures::path("many-entries.tar"), out / "b", few_entries);
    CHECK(fixtures::has_issue(many, ErrorCode::LimitExceeded));
    CHECK(many.files == 10);

    Options listing;
    listing.limits.max_entries = 10;
    CHECK_THROWS_AS(list(fixtures::path("many-entries.tar"), listing), Error);

    const Report deep = extract(fixtures::path("deep.tar"), out / "c");
    CHECK(fixtures::has_issue(deep, ErrorCode::LimitExceeded));
}

TEST_CASE("decoders respect the memory limit")
{
    Options options;
    options.limits.max_memory = 1 << 10;
    const Report report = test(fixtures::path("basic.tar.xz"), options);
    CHECK(fixtures::has_issue(report, ErrorCode::LimitExceeded));
}

TEST_CASE("corrupt archives fail cleanly without partial files")
{
    const auto [name, code] = GENERATE(table<std::string, ErrorCode>({
        {"corrupt-checksum.tar", ErrorCode::Unsupported},
        {"truncated.tar", ErrorCode::Corrupt},
        {"truncated.tar.gz", ErrorCode::Corrupt},
        {"bad-crc.gz", ErrorCode::Checksum},
        {"truncated.xz", ErrorCode::Corrupt},
        {"truncated.zst", ErrorCode::Corrupt},
    }));
    CAPTURE(name);
    TempDir out;
    const Report report = extract(fixtures::path(name), out.path());

    CHECK_FALSE(report.ok());
    CHECK(fixtures::has_issue(report, code));
    CHECK_FALSE(fixtures::has_temp_leftovers(out.path()));
    CHECK_FALSE(test(fixtures::path(name)).ok());
}

TEST_CASE("a truncated tar keeps the entries completed before the damage")
{
    TempDir out;
    const Report report = extract(fixtures::path("truncated.tar"), out.path());
    CHECK(fixtures::has_issue(report, ErrorCode::Corrupt));
    CHECK(fixtures::read_file(out / "hello.txt") == fixtures::bytes("Hello, DigZip!\n"));
    CHECK_FALSE(fixtures::has_temp_leftovers(out.path()));
}

TEST_CASE("the destination must be a directory")
{
    TempDir dir;
    fixtures::write_file(dir / "file", "x");
    const Report report = extract(fixtures::path("basic.tar"), dir / "file");
    CHECK(fixtures::has_issue(report, ErrorCode::InvalidArgument));
}

TEST_CASE("damaged archives of every format never crash the engine")
{
    const std::string name = GENERATE("basic.tar.gz", "basic.tar.xz", "basic.tar.zst", "basic.tar.bz2",
                                      "basic-deflate.zip", "basic-deflate64.zip", "basic-lzma.zip", "ppmd.zip",
                                      "basic.7z", "bcj2.7z", "deflate64.7z", "ppmd.7z", "ppmd-o2.zip");
    CAPTURE(name);
    const auto original = fixtures::read_file(fixtures::path(name));
    REQUIRE(original.size() > 64);
    TempDir dir;
    const fs::path damaged = dir / "damaged.bin";
    for (std::size_t step = 0; step < 60; ++step) {
        auto bytes = original;
        const std::size_t position = (original.size() - 1) * step / 59;
        bytes[position] ^= static_cast<std::uint8_t>(0x5A + step);
        {
            std::ofstream out(damaged, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        CAPTURE(position);
        const Report report = test(damaged);
        for (const Issue& issue : report.issues) {
            CHECK_FALSE(issue.message.empty());
        }
    }
}

TEST_CASE("headers announcing absurd counts or sizes are refused without allocating")
{
    const auto [name, code] = GENERATE(table<std::string, ErrorCode>({
        {"hostile-zip-count.zip", ErrorCode::Corrupt},
        {"hostile-7z-count.7z", ErrorCode::Corrupt},
        {"hostile-tar-pax.tar", ErrorCode::Corrupt},
        {"hostile-gz-name.gz", ErrorCode::Corrupt},
    }));
    CAPTURE(name);
    const Report report = test(fixtures::path(name));
    CHECK(fixtures::has_issue(report, code));
}

TEST_CASE("an empty file is reported as an error")
{
    CHECK_FALSE(test(fixtures::path("empty.bin")).ok());
    CHECK_THROWS_AS(list(fixtures::path("empty.bin")), Error);
}
