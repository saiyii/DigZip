#include "codec/blake2sp.hpp"
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

std::vector<std::pair<std::string, std::vector<std::uint8_t>>> files_of_basic()
{
    auto files = fixtures::basic_files();
    files.erase(std::remove_if(files.begin(), files.end(), [](const auto& f) { return f.first == "bin/run.sh"; }),
                files.end());
    return files;
}

std::string hex(const std::array<std::uint8_t, 32>& digest)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t b : digest) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

} // namespace

TEST_CASE("BLAKE2sp matches the reference values")
{
    codec::Blake2sp empty;
    CHECK(hex(empty.finish()) == "dd0e891776933f43c7d032b08a917e25741f8aa9a12c12e1cac8801500f2ca4f");

    // Same input fed in one call and in odd-sized pieces.
    const auto data = fixtures::lcg_bytes(5000);
    codec::Blake2sp whole;
    whole.update(data.data(), data.size());
    codec::Blake2sp pieces;
    for (std::size_t i = 0; i < data.size(); i += 333) {
        pieces.update(data.data() + i, std::min<std::size_t>(333, data.size() - i));
    }
    CHECK(whole.finish() == pieces.finish());
}

TEST_CASE("RAR 5 archives are listed")
{
    const ArchiveInfo info = list(fixtures::path("basic.rar"));
    CHECK(to_string(info.format) == "rar");
    const auto entries = by_path(info);
    CHECK(entries.at("hello.txt").size == 15u);
    CHECK(entries.at("hello.txt").crc32 == 0xDD4C15FBu);
    CHECK(entries.at("hello.txt").mtime->seconds == 1'700'000'000);
    CHECK(entries.at("docs/readme.md").method == "rar5");
    CHECK(entries.at("data/empty_dir").type == EntryType::Directory);
}

TEST_CASE("RAR 5 archives extract exactly")
{
    const std::string name = GENERATE("basic.rar", "stored.rar", "best-solid.rar", "blake2.rar");
    CAPTURE(name);
    TempDir out;
    const Report report = extract(fixtures::path(name), out.path());
    CHECK(report.ok());
    for (const auto& [path, content] : files_of_basic()) {
        CAPTURE(path);
        CHECK(fixtures::read_file(out / path) == content);
    }
    CHECK(test(fixtures::path(name)).ok());
}

TEST_CASE("RAR 5 solid archives, small dictionaries and filters")
{
    TempDir out;
    REQUIRE(extract(fixtures::path("solid.rar"), out / "solid").ok());
    for (std::uint32_t i = 0; i < 6; ++i) {
        CHECK(fixtures::read_file(out / ("solid/part" + std::to_string(i) + ".txt")) == fixtures::word_text(4000, i));
    }

    REQUIRE(extract(fixtures::path("large.rar"), out / "large").ok());
    CHECK(fixtures::read_file(out / "large/large.txt") == fixtures::word_text(120'000));

    // A 128 KiB dictionary much smaller than the data.
    REQUIRE(extract(fixtures::path("small-dict.rar"), out / "small").ok());
    CHECK(fixtures::read_file(out / "small/large.txt") == fixtures::word_text(120'000));
    CHECK(fixtures::read_file(out / "small/random.bin") == fixtures::lcg_bytes(30'000));

    REQUIRE(extract(fixtures::path("x86.rar"), out / "x86").ok());
    CHECK(fixtures::read_file(out / "x86/x86.bin") == fixtures::x86_like(60'000));

    REQUIRE(extract(fixtures::path("delta.rar"), out / "delta").ok());
    std::vector<std::uint8_t> samples;
    for (int i = 0; i < 120'000; ++i) {
        samples.push_back(static_cast<std::uint8_t>((i * 7 + (i / 4) * 3) & 0xFF));
    }
    CHECK(fixtures::read_file(out / "delta/samples.bin") == samples);
}

TEST_CASE("RAR 5 encryption, including encrypted file names")
{
    const std::string name = GENERATE("encrypted.rar", "encrypted-headers.rar");
    CAPTURE(name);
    TempDir out;

    ExtractOptions options;
    options.passwords = {"wrong", "secret"};
    REQUIRE(extract(fixtures::path(name), out / "ok", options).ok());
    CHECK(fixtures::read_file(out / "ok/secret.txt") == fixtures::bytes("top secret\n"));
    CHECK(fixtures::read_file(out / "ok/notes.txt") == fixtures::word_text(3000));

    CHECK(fixtures::has_issue(extract(fixtures::path(name), out / "none"), ErrorCode::PasswordRequired));
    ExtractOptions wrong;
    wrong.passwords = {"nope"};
    CHECK(fixtures::has_issue(extract(fixtures::path(name), out / "wrong", wrong), ErrorCode::WrongPassword));
    CHECK_FALSE(fs::exists(out / "wrong/secret.txt"));
}

TEST_CASE("RAR 5 links and archives written by DigZip's crafted writer")
{
    TempDir out;
    REQUIRE(extract(fixtures::path("crafted.rar"), out / "c").ok());
    CHECK(fixtures::read_file(out / "c/a.txt") == fixtures::bytes("first\n"));
    CHECK(fixtures::read_file(out / "c/b/c.txt") == fixtures::bytes("second\n"));

    const auto entries = by_path(list(fixtures::path("links.rar")));
    CHECK(entries.at("hello-hard").type == EntryType::Hardlink);
    CHECK(entries.at("hello-link").type == EntryType::Symlink);
    CHECK(entries.at("hello-link").link_target == "hello.txt");
    extract(fixtures::path("links.rar"), out / "l");
    CHECK(fixtures::read_file(out / "l/hello-hard") == fixtures::bytes("Hello, DigZip!\n"));
    CHECK(fixtures::read_file(out / "l/hello-link") == fixtures::bytes("Hello, DigZip!\n"));
}

TEST_CASE("hostile RAR entries are refused, the others extracted")
{
    const std::string name = GENERATE("evil-traversal.rar", "evil-absolute.rar", "evil-symlink.rar");
    CAPTURE(name);
    TempDir dir;
    const Report report = extract(fixtures::path(name), dir / "out");
    CHECK(fixtures::has_issue(report, ErrorCode::Security));
    CHECK(fixtures::tree(dir.path()) == std::vector<std::string>{"out", "out/safe.txt"});
}

TEST_CASE("damaged and unsupported RAR archives are reported")
{
    const auto [name, code] = GENERATE(table<std::string, ErrorCode>({
        {"truncated.rar", ErrorCode::Corrupt},
        {"corrupt-data.rar", ErrorCode::Checksum},
        {"volumes.part1.rar", ErrorCode::Unsupported},
        {"rar4-signature.rar", ErrorCode::Unsupported},
    }));
    CAPTURE(name);
    TempDir out;
    const Report report = extract(fixtures::path(name), out.path());
    CHECK(fixtures::has_issue(report, code));
    CHECK_FALSE(fixtures::has_temp_leftovers(out.path()));
}
