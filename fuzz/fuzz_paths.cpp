// Fuzzing of the path rules: whatever the input, a sanitized path never leaves the destination.
// Input: one flag byte (Windows rules), then the entry path and a link target separated by a NUL.

#include "extract/path_rules.hpp"

#include <digzip/error.hpp>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

void check_component(const std::string& c, bool windows)
{
    const bool bad = c.empty() || c == "." || c == ".." || c.find_first_of(std::string("/\\\0", 3)) != std::string::npos ||
                     (windows && c.find_first_of("<>:\"|?*") != std::string::npos);
    if (bad) {
        std::abort();
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    using namespace digzip;
    if (size < 1) {
        return 0;
    }
    const bool windows = (data[0] & 1) != 0;
    const std::string_view text(reinterpret_cast<const char*>(data + 1), size - 1);
    const std::size_t split = text.find('\0');
    const std::string_view path = text.substr(0, split);
    const std::string_view target = split == std::string_view::npos ? std::string_view() : text.substr(split + 1);

    const extraction::NameRules rules{windows, 64};
    try {
        const extraction::SafePath safe = extraction::sanitize_path(path, rules);
        for (const std::string& c : safe.components) {
            check_component(c, windows);
        }
        if (!safe.components.empty()) {
            for (const std::string& c : extraction::resolve_link_target(safe.components, target)) {
                if (c == ".." || c == "." || c.empty()) {
                    std::abort();
                }
            }
        }
    } catch (const Error&) {
    }
    return 0;
}
