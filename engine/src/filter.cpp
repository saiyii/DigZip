#include "filter.hpp"

namespace digzip {

namespace {

// A pattern matches a path, or any directory that contains it.
bool matches_path_or_parent(std::string_view pattern, std::string_view path)
{
    if (glob_match(pattern, path)) {
        return true;
    }
    for (std::size_t slash = path.find('/'); slash != std::string_view::npos; slash = path.find('/', slash + 1)) {
        if (glob_match(pattern, path.substr(0, slash))) {
            return true;
        }
    }
    return false;
}

bool matches_any(const std::vector<std::string>& patterns, std::string_view path)
{
    for (const std::string& pattern : patterns) {
        if (matches_path_or_parent(pattern, path)) {
            return true;
        }
    }
    return false;
}

} // namespace

bool glob_match(std::string_view pattern, std::string_view text)
{
    // Iterative matching with backtracking to the last '*': linear in practice, no recursion.
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t star = std::string_view::npos;
    std::size_t star_text = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            star_text = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++star_text;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

bool is_selected(std::string_view path, const Options& options)
{
    if (!options.include.empty() && !matches_any(options.include, path)) {
        return false;
    }
    return !matches_any(options.exclude, path);
}

} // namespace digzip
