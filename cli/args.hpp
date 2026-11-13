#pragma once

#include <digzip/archive.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace cli {

enum class Action { Help, Version, List, Extract, Test };

struct Command {
    Action action = Action::Help;
    std::filesystem::path archive;
    std::filesystem::path output = ".";
    digzip::ExtractOptions options;
    bool json = false;
    bool quiet = false;
    bool progress = true;
};

class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Arguments without the program name, in UTF-8. Throws UsageError.
Command parse(const std::vector<std::string>& args);

const char* help_text();

} // namespace cli
