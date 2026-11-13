#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace digzip::extraction {

struct NameRules {
    bool windows = false; // apply Windows naming restrictions
    std::uint32_t max_depth = 256;
};

NameRules host_rules(std::uint32_t max_depth);

struct SafePath {
    std::vector<std::string> components; // never empty strings, ".", or ".."
    bool renamed = false;                // a component was changed to be valid on this system
};

// Turns an archive path into components that stay inside the destination.
// Throws Error(Security) for absolute paths, drive letters, ".." and NUL characters,
// Error(InvalidName) for names too long for the file system,
// Error(LimitExceeded) past `max_depth` levels.
SafePath sanitize_path(std::string_view path, const NameRules& rules);

// Resolves a symbolic link target relative to the directory holding `link`.
// Returns the components of the resolved path, relative to the destination root.
// Throws Error(Security) if the target is absolute or leaves the destination.
std::vector<std::string> resolve_link_target(const std::vector<std::string>& link, std::string_view target);

std::string join(const std::vector<std::string>& components, std::size_t count);

} // namespace digzip::extraction
