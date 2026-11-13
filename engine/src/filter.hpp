#pragma once

#include <digzip/archive.hpp>

#include <string_view>

namespace digzip {

// '*' matches any sequence (including '/'), '?' any single character. Case-sensitive.
bool glob_match(std::string_view pattern, std::string_view text);

// True if the entry path passes Options::include and Options::exclude.
bool is_selected(std::string_view path, const Options& options);

} // namespace digzip
