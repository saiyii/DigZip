#include "args.hpp"
#include "output.hpp"
#include "platform.hpp"

#include <digzip/archive.hpp>

#include <iostream>
#include <string>
#include <vector>

namespace {

int run(const std::vector<std::string>& args)
{
    cli::setup_console();

    cli::Command command;
    try {
        command = cli::parse(args);
    } catch (const cli::UsageError& e) {
        std::cerr << "digzip: " << e.what() << "\nTry 'digzip --help'.\n";
        return cli::kUsage;
    }

    switch (command.action) {
    case cli::Action::Help:
        std::cout << cli::help_text();
        return cli::kOk;
    case cli::Action::Version:
        cli::print_version(std::cout, command.json);
        return cli::kOk;
    default:
        break;
    }

    command.options.cancel = &cli::install_interrupt_handler();
    cli::ProgressBar bar;
    if (command.progress && !command.quiet && !command.json && cli::stderr_is_terminal()) {
        command.options.on_progress = [&bar](const digzip::Progress& p) { bar.update(p); };
    }

    if (command.action == cli::Action::List) {
        digzip::ArchiveInfo info;
        try {
            info = digzip::list(command.archive, command.options);
        } catch (const digzip::Error& e) {
            bar.clear();
            if (command.json) {
                cli::print_error_json(std::cout, command.archive, e);
            } else {
                std::cerr << "error: " << e.what() << '\n';
            }
            return cli::exit_code(e.code());
        }
        bar.clear();
        if (command.json) {
            cli::print_list_json(std::cout, command.archive, info);
        } else {
            cli::print_list(std::cout, info);
        }
        return cli::kOk;
    }

    const bool extracting = command.action == cli::Action::Extract;
    const std::string action = extracting ? "extract" : "test";
    const digzip::Report report = extracting ? digzip::extract(command.archive, command.output, command.options)
                                             : digzip::test(command.archive, command.options);
    bar.clear();
    if (command.json) {
        cli::print_report_json(std::cout, command.archive, action, report);
    } else {
        cli::print_report(std::cout, std::cerr, action, report, command.quiet);
    }
    return cli::exit_code(report);
}

} // namespace

#ifdef _WIN32
// Arguments arrive as UTF-16 and are converted to UTF-8, so non-ASCII paths and passwords work.
int wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.push_back(cli::to_utf8(argv[i]));
    }
    return run(args);
}
#else
int main(int argc, char** argv)
{
    return run(std::vector<std::string>(argv + 1, argv + argc));
}
#endif
