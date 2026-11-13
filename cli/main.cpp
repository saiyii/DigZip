#include <digzip/version.hpp>

#include <iostream>
#include <string_view>

// Placeholder entry point: the full CLI (list, extract, test, --json) comes in v0.x phase 4.
int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        const auto info = digzip::version_info();
        std::cout << "digzip " << info.program << '\n'
                  << "api " << info.api << '\n';
        for (const auto& dep : info.dependencies) {
            std::cout << dep.name << ' ' << dep.version << '\n';
        }
        return 0;
    }

    std::cerr << "usage: digzip --version\n";
    return 2;
}
