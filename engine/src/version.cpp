#include "crypto/crypto.hpp"

#include <digzip/version.hpp>

#include <bzlib.h>
#include <lzma.h>
#include <zlib.h>
#include <zstd.h>

namespace digzip {

namespace {

// BZ2_bzlibVersion() returns "1.0.8, 13-Jul-2019": keep the version only.
std::string bzip2_version()
{
    std::string version = BZ2_bzlibVersion();
    return version.substr(0, version.find(','));
}

} // namespace

VersionInfo version_info()
{
    return {
        DIGZIP_VERSION_STRING,
        DIGZIP_API_VERSION,
        {
            {"zlib", zlibVersion()},
            {"bzip2", bzip2_version()},
            {"liblzma", lzma_version_string()},
            {"zstd", ZSTD_versionString()},
            {crypto::backend().first, crypto::backend().second},
        },
    };
}

} // namespace digzip
