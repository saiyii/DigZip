#include <digzip/version.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace {

bool has_dependency(const digzip::VersionInfo& info, const std::string& name)
{
    return std::any_of(info.dependencies.begin(), info.dependencies.end(),
                       [&](const digzip::Dependency& d) { return d.name == name; });
}

} // namespace

TEST_CASE("version_info reports the program and API versions")
{
    const auto info = digzip::version_info();
    CHECK(info.program == DIGZIP_EXPECTED_VERSION);
    CHECK(info.api == DIGZIP_EXPECTED_API_VERSION);
}

TEST_CASE("version_info lists every codec library required by v0.x")
{
    const auto info = digzip::version_info();
    CHECK(has_dependency(info, "zlib"));
    CHECK(has_dependency(info, "bzip2"));
    CHECK(has_dependency(info, "liblzma"));
    CHECK(has_dependency(info, "zstd"));
}

TEST_CASE("every dependency has a non-empty name and a clean version")
{
    for (const auto& dep : digzip::version_info().dependencies) {
        CHECK_FALSE(dep.name.empty());
        CHECK_FALSE(dep.version.empty());
        CHECK(dep.version.find_first_of(", ") == std::string::npos);
    }
}
