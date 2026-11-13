#include "extract/path_rules.hpp"
#include "text.hpp"

#include <digzip/error.hpp>

#include <algorithm>
#include <cctype>

namespace digzip::extraction {

namespace {

constexpr std::size_t kMaxComponentLength = 255;

bool is_separator(char c)
{
    return c == '/' || c == '\\';
}

bool has_drive_letter(std::string_view path)
{
    return path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':';
}

bool is_absolute(std::string_view path)
{
    return (!path.empty() && is_separator(path[0])) || has_drive_letter(path);
}

template <typename Visitor>
void for_each_component(std::string_view path, Visitor&& visit)
{
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = start;
        while (end < path.size() && !is_separator(path[end])) {
            ++end;
        }
        const std::string_view part = path.substr(start, end - start);
        if (!part.empty() && part != ".") {
            visit(part);
        }
        start = end + 1;
    }
}

std::string upper_ascii(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

// CON, PRN, AUX, NUL, COM0-9, LPT0-9 (and the superscript digits ¹ ² ³), CONIN$, CONOUT$.
bool is_reserved_device(std::string_view base)
{
    const std::string name = upper_ascii(base);
    if (name == "CON" || name == "PRN" || name == "AUX" || name == "NUL" || name == "CONIN$" || name == "CONOUT$") {
        return true;
    }
    if (name.size() < 4 || (name.compare(0, 3, "COM") != 0 && name.compare(0, 3, "LPT") != 0)) {
        return false;
    }
    const std::string_view digit = std::string_view(name).substr(3);
    return (digit.size() == 1 && digit[0] >= '0' && digit[0] <= '9') || digit == "\xC2\xB9" ||
           digit == "\xC2\xB2" || digit == "\xC2\xB3";
}

std::string windows_component(std::string_view part, bool& renamed)
{
    std::string out(part);
    for (char& c : out) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || std::string_view("<>:\"|?*").find(c) != std::string_view::npos) {
            c = '_';
            renamed = true;
        }
    }
    // Windows silently drops trailing dots and spaces, which would merge distinct names.
    for (auto it = out.rbegin(); it != out.rend() && (*it == '.' || *it == ' '); ++it) {
        *it = '_';
        renamed = true;
    }
    std::size_t base_end = out.find('.');
    if (base_end == std::string::npos) {
        base_end = out.size();
    }
    std::size_t trimmed = base_end;
    while (trimmed > 0 && out[trimmed - 1] == ' ') {
        --trimmed;
    }
    if (is_reserved_device(std::string_view(out).substr(0, trimmed))) {
        out.insert(base_end, "_");
        renamed = true;
    }
    return out;
}

} // namespace

NameRules host_rules(std::uint32_t max_depth)
{
    NameRules rules;
#ifdef _WIN32
    rules.windows = true;
#endif
    rules.max_depth = max_depth;
    return rules;
}

SafePath sanitize_path(std::string_view path, const NameRules& rules)
{
    if (path.find('\0') != std::string_view::npos) {
        throw Error(ErrorCode::Security, "the name contains a NUL character");
    }
    if (is_absolute(path)) {
        throw Error(ErrorCode::Security, "absolute paths are not allowed");
    }

    SafePath safe;
    for_each_component(path, [&](std::string_view part) {
        if (part == "..") {
            throw Error(ErrorCode::Security, "the path contains '..'");
        }
        std::string component = rules.windows ? windows_component(part, safe.renamed) : std::string(part);
        const std::size_t length = rules.windows ? text::utf16_length(component) : component.size();
        if (length > kMaxComponentLength) {
            throw Error(ErrorCode::InvalidName, "a path component is longer than 255 characters");
        }
        safe.components.push_back(std::move(component));
    });

    if (rules.max_depth != 0 && safe.components.size() > rules.max_depth) {
        throw Error(ErrorCode::LimitExceeded,
                    "the path is nested more than " + std::to_string(rules.max_depth) + " levels deep (limit: max_path_depth)");
    }
    return safe;
}

std::vector<std::string> resolve_link_target(const std::vector<std::string>& link, std::string_view target)
{
    if (target.empty()) {
        throw Error(ErrorCode::Corrupt, "the symbolic link has an empty target");
    }
    if (target.find('\0') != std::string_view::npos) {
        throw Error(ErrorCode::Security, "the link target contains a NUL character");
    }
    if (is_absolute(target)) {
        throw Error(ErrorCode::Security, "the symbolic link points to an absolute path");
    }
    std::vector<std::string> resolved(link.begin(), link.empty() ? link.end() : link.end() - 1);
    for_each_component(target, [&](std::string_view part) {
        if (part != "..") {
            resolved.emplace_back(part);
        } else if (resolved.empty()) {
            throw Error(ErrorCode::Security, "the symbolic link points outside the destination");
        } else {
            resolved.pop_back();
        }
    });
    return resolved;
}

std::string join(const std::vector<std::string>& components, std::size_t count)
{
    std::string out;
    for (std::size_t i = 0; i < count && i < components.size(); ++i) {
        if (i > 0) {
            out += '/';
        }
        out += components[i];
    }
    return out;
}

} // namespace digzip::extraction
