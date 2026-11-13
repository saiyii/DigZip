#pragma once

#include <digzip/export.hpp>

#include <string>
#include <vector>

namespace digzip {

struct Dependency {
    std::string name;
    std::string version;
};

struct VersionInfo {
    std::string program;                  // SemVer, e.g. "0.1.0"
    int api = 0;                          // 0 while the API is unstable (v0.x)
    std::vector<Dependency> dependencies; // versions linked at run time
};

DIGZIP_EXPORT VersionInfo version_info();

} // namespace digzip
