#include "extract/path_rules.hpp"

#include <digzip/error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

using digzip::Error;
using digzip::ErrorCode;
using digzip::extraction::NameRules;
using digzip::extraction::resolve_link_target;
using digzip::extraction::sanitize_path;

namespace {

const NameRules posix{false, 256};
const NameRules windows{true, 256};

std::optional<ErrorCode> error_of(const std::string& path, const NameRules& rules = posix)
{
    try {
        sanitize_path(path, rules);
    } catch (const Error& e) {
        return e.code();
    }
    return std::nullopt;
}

std::string clean(const std::string& path, const NameRules& rules = posix)
{
    const auto safe = sanitize_path(path, rules);
    return digzip::extraction::join(safe.components, safe.components.size());
}

} // namespace

TEST_CASE("ordinary paths are kept")
{
    CHECK(clean("a/b/c.txt") == "a/b/c.txt");
    CHECK(clean("./a//b/./c") == "a/b/c");
    CHECK(clean("dir/") == "dir");
    CHECK(clean("caf\xC3\xA9.txt") == "caf\xC3\xA9.txt");
    CHECK(sanitize_path(".", posix).components.empty());
}

TEST_CASE("backslashes are separators on every system")
{
    CHECK(clean("a\\b\\c.txt") == "a/b/c.txt");
}

TEST_CASE("paths leaving the destination are refused")
{
    CHECK(error_of("../evil") == ErrorCode::Security);
    CHECK(error_of("a/../../evil") == ErrorCode::Security);
    CHECK(error_of("a/../b") == ErrorCode::Security);
    CHECK(error_of("..\\evil") == ErrorCode::Security);
    CHECK(error_of("/etc/passwd") == ErrorCode::Security);
    CHECK(error_of("\\\\server\\share\\x") == ErrorCode::Security);
    CHECK(error_of("C:/Windows/evil") == ErrorCode::Security);
    CHECK(error_of("c:evil") == ErrorCode::Security);
    CHECK(error_of("a:b.txt") == ErrorCode::Security); // drive-relative on Windows
    CHECK(error_of(std::string("a\0b", 3)) == ErrorCode::Security);
}

TEST_CASE("Windows rules fix forbidden characters and reserved names")
{
    CHECK(clean("ab:c.txt", windows) == "ab_c.txt");
    CHECK(clean("q?<>|\"*.txt", windows) == "q______.txt");
    CHECK(clean("con.txt", windows) == "con_.txt");
    CHECK(clean("AUX", windows) == "AUX_");
    CHECK(clean("dir/COM1.log", windows) == "dir/COM1_.log");
    CHECK(clean("nul .txt", windows) == "nul _.txt");
    CHECK(clean("LPT\xC2\xB9", windows) == "LPT\xC2\xB9_");
    CHECK(clean("trailing. ", windows) == "trailing__");
    CHECK(clean("console.txt", windows) == "console.txt");
    CHECK(sanitize_path("con.txt", windows).renamed);
    CHECK_FALSE(sanitize_path("plain.txt", windows).renamed);
}

TEST_CASE("POSIX rules keep names that are only invalid on Windows")
{
    CHECK(clean("ab:c.txt") == "ab:c.txt");
    CHECK(clean("con.txt") == "con.txt");
}

TEST_CASE("over-long names and over-deep paths are refused")
{
    CHECK(error_of(std::string(256, 'x')) == ErrorCode::InvalidName);
    CHECK(clean(std::string(255, 'x')).size() == 255);
    // 200 characters of 2 bytes each: too long in bytes, fine in UTF-16 code units.
    std::string accented;
    for (int i = 0; i < 200; ++i) {
        accented += "\xC3\xA9";
    }
    CHECK(error_of(accented) == ErrorCode::InvalidName);
    CHECK(sanitize_path(accented, windows).components.size() == 1);

    std::string deep;
    for (int i = 0; i < 10; ++i) {
        deep += "d/";
    }
    CHECK(error_of(deep + "f", NameRules{false, 10}) == ErrorCode::LimitExceeded);
    CHECK(sanitize_path(deep, NameRules{false, 10}).components.size() == 10);
}

TEST_CASE("symbolic link targets must stay inside the destination")
{
    using V = std::vector<std::string>;
    CHECK(resolve_link_target({"links", "l"}, "../hello.txt") == V{"hello.txt"});
    CHECK(resolve_link_target({"l"}, "a/./b") == V{"a", "b"});
    CHECK(resolve_link_target({"a", "b", "l"}, "../../c") == V{"c"});

    const auto refused = [](const V& link, const std::string& target) {
        try {
            resolve_link_target(link, target);
        } catch (const Error& e) {
            return e.code() == ErrorCode::Security;
        }
        return false;
    };
    CHECK(refused({"l"}, ".."));
    CHECK(refused({"a", "l"}, "../.."));
    CHECK(refused({"l"}, "a/../../x"));
    CHECK(refused({"l"}, "/etc"));
    CHECK(refused({"l"}, "C:\\Windows"));
    CHECK(refused({"l"}, "..\\x"));
}
