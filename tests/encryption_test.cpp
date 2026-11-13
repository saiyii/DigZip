#include "crypto/crypto.hpp"
#include "support/fixtures.hpp"

#include <digzip/archive.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace digzip;
using fixtures::TempDir;
namespace fs = std::filesystem;

namespace {

std::string hex(const std::uint8_t* data, std::size_t size)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < size; ++i) {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 15];
    }
    return out;
}

std::vector<std::uint8_t> unhex(const std::string& text)
{
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(std::stoi(text.substr(i, 2), nullptr, 16)));
    }
    return out;
}

const std::uint8_t* bytes_of(const std::string& s)
{
    return reinterpret_cast<const std::uint8_t*>(s.data());
}

void check_secret_files(const fs::path& root)
{
    CHECK(fixtures::read_file(root / "secret.txt") == fixtures::bytes("top secret\n"));
    CHECK(fixtures::read_file(root / "notes.txt") == fixtures::word_text(3000));
    CHECK(fixtures::read_file(root / "data/random.bin") == fixtures::lcg_bytes(20000));
}

} // namespace

TEST_CASE("system cryptography matches the published test vectors")
{
    // FIPS-197 appendix C.1 (AES-128) and C.3 (AES-256).
    const auto plain = unhex("00112233445566778899aabbccddeeff");
    std::uint8_t out[16];
    const auto key128 = unhex("000102030405060708090a0b0c0d0e0f");
    crypto::Aes aes128(key128.data(), key128.size());
    aes128.encrypt(plain.data(), out, 1);
    CHECK(hex(out, 16) == "69c4e0d86a7b0430d8cdb78070b4c55a");
    aes128.decrypt(out, out, 1);
    CHECK(hex(out, 16) == "00112233445566778899aabbccddeeff");

    const auto key256 = unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    crypto::Aes aes256(key256.data(), key256.size());
    aes256.encrypt(plain.data(), out, 1);
    CHECK(hex(out, 16) == "8ea2b7ca516745bfeafc49904b496089");

    crypto::Sha256 sha;
    sha.update(bytes_of("abc"), 3);
    const auto digest = sha.finish();
    CHECK(hex(digest.data(), digest.size()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // RFC 2202, test case 2.
    const std::string key = "Jefe";
    const std::string data = "what do ya want for nothing?";
    crypto::Hmac hmac(crypto::Hash::Sha1, bytes_of(key), key.size());
    hmac.update(bytes_of(data), data.size());
    const auto mac = hmac.finish();
    CHECK(hex(mac.data(), mac.size()) == "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");

    // RFC 4231, test case 2.
    crypto::Hmac hmac256(crypto::Hash::Sha256, bytes_of(key), key.size());
    hmac256.update(bytes_of(data), data.size());
    const auto mac256 = hmac256.finish();
    CHECK(hex(mac256.data(), mac256.size()) ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    // RFC 6070 (PBKDF2-HMAC-SHA1) and RFC 7914 section 11 (PBKDF2-HMAC-SHA256).
    std::uint8_t derived[20];
    crypto::pbkdf2(crypto::Hash::Sha1, bytes_of("password"), 8, bytes_of("salt"), 4, 2, derived, sizeof derived);
    CHECK(hex(derived, sizeof derived) == "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957");
    crypto::pbkdf2(crypto::Hash::Sha1, bytes_of("password"), 8, bytes_of("salt"), 4, 4096, derived, sizeof derived);
    CHECK(hex(derived, sizeof derived) == "4b007901b765489abead49d926f721d065a429c1");
    std::uint8_t derived256[64];
    crypto::pbkdf2(crypto::Hash::Sha256, bytes_of("passwd"), 6, bytes_of("salt"), 4, 1, derived256, sizeof derived256);
    CHECK(hex(derived256, sizeof derived256) ==
          "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
          "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783");
}

TEST_CASE("encrypted archives extract with the right password")
{
    const std::string name =
        GENERATE("zipcrypto.zip", "zipcrypto-stored.zip", "aes128.zip", "aes192.zip", "aes256.zip",
                 "aes256-stored.zip", "encrypted.7z", "encrypted-header.7z", "encrypted-non-solid.7z",
                 "encrypted-copy.7z");
    CAPTURE(name);
    ExtractOptions options;
    options.passwords = {"secret"};
    TempDir out;
    const Report report = extract(fixtures::path(name), out.path(), options);
    CHECK(report.ok());
    check_secret_files(out.path());
    CHECK(test(fixtures::path(name), options).ok());
}

TEST_CASE("passwords are tried in order")
{
    const std::string name = GENERATE("zipcrypto.zip", "aes256.zip", "encrypted.7z", "encrypted-header.7z",
                                      "encrypted-copy.7z");
    CAPTURE(name);
    ExtractOptions options;
    options.passwords = {"wrong", "also wrong", "secret"};
    TempDir out;
    CHECK(extract(fixtures::path(name), out.path(), options).ok());
    check_secret_files(out.path());
}

TEST_CASE("a non-ASCII password works through UTF-16 in 7z")
{
    ExtractOptions options;
    options.passwords = {"p\xC3\xA4ssw\xC3\xB6rd \xE2\x98\x95"};
    TempDir out;
    CHECK(extract(fixtures::path("encrypted-unicode.7z"), out.path(), options).ok());
    check_secret_files(out.path());
}

TEST_CASE("missing and wrong passwords are reported, without leaking them")
{
    const std::string name = GENERATE("zipcrypto.zip", "aes256.zip", "encrypted.7z", "encrypted-header.7z");
    CAPTURE(name);
    TempDir out;

    const Report missing = extract(fixtures::path(name), out / "missing");
    CHECK(fixtures::has_issue(missing, ErrorCode::PasswordRequired));
    CHECK_FALSE(fixtures::has_temp_leftovers(out.path()));

    ExtractOptions options;
    options.passwords = {"Sup3rS3cretGuess!"};
    const Report wrong = extract(fixtures::path(name), out / "wrong", options);
    CHECK(fixtures::has_issue(wrong, ErrorCode::WrongPassword));
    for (const Issue& issue : wrong.issues) {
        CHECK(issue.message.find("Sup3rS3cretGuess!") == std::string::npos);
    }
    CHECK_FALSE(fs::exists(out / "wrong/secret.txt"));
}

TEST_CASE("encrypted zip entries can be listed without a password")
{
    const ArchiveInfo info = list(fixtures::path("aes128.zip"));
    REQUIRE(info.entries.size() == 3);
    for (const Entry& e : info.entries) {
        CHECK(e.encrypted);
        CHECK(e.method.find("+aes128") != std::string::npos);
    }
    CHECK_THROWS_AS(list(fixtures::path("encrypted-header.7z")), Error);
}
