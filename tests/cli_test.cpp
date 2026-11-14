#include "support/fixtures.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdlib>

#ifndef _WIN32
#include <sys/wait.h>
#endif

using fixtures::TempDir;
namespace fs = std::filesystem;

namespace {

struct Result {
    int exit_code = -1;
    std::string out;
    std::string err;
};

std::string quote(const std::string& s)
{
    return "\"" + s + "\"";
}

std::string text_of(const fs::path& file)
{
    const auto bytes = fixtures::read_file(file);
    return {bytes.begin(), bytes.end()};
}

void set_env(const char* name, const std::string& value);

// Runs the digzip executable with `args` (already quoted where needed), in `language`.
Result run(const std::string& args, const std::string& language = "en")
{
    set_env("DIGZIP_LANG", language);
    static TempDir dir;
    const fs::path out = dir / "stdout.txt";
    const fs::path err = dir / "stderr.txt";
    std::string command = quote(DIGZIP_CLI_PATH) + " " + args + " > " + quote(out.string()) + " 2> " + quote(err.string());
#ifdef _WIN32
    command = "\"" + command + "\""; // cmd.exe strips one level of quotes
#endif
    const int status = std::system(command.c_str());
    Result result;
#ifdef _WIN32
    result.exit_code = status;
#else
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    result.out = text_of(out);
    result.err = text_of(err);
    return result;
}

std::string fixture(const std::string& name)
{
    return quote(fixtures::path(name).string());
}

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

void set_env(const char* name, const std::string& value)
{
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

} // namespace

TEST_CASE("cli: version and help")
{
    const Result version = run("--version --json");
    CHECK(version.exit_code == 0);
    CHECK(contains(version.out, "\"digzip\":\"" DIGZIP_EXPECTED_VERSION "\""));
    CHECK(contains(version.out, "\"zlib\":"));

    const Result help = run("--help");
    CHECK(help.exit_code == 0);
    CHECK(contains(help.out, "Exit codes:"));

    const Result nothing = run("");
    CHECK(nothing.exit_code == 0);
}

TEST_CASE("cli: messages in French")
{
    const Result help = run("--help", "fr");
    CHECK(contains(help.out, "Codes de retour :"));

    const Result usage = run("frobnicate x.zip", "fr");
    CHECK(usage.exit_code == 2);
    CHECK(contains(usage.err, "commande inconnue « frobnicate »"));
    CHECK(contains(usage.err, "Essayez « digzip --help »."));

    const Result listing = run("list " + fixture("basic.tar"), "fr");
    CHECK(listing.exit_code == 0);
    CHECK(contains(listing.out, "Modifié (UTC)"));
    CHECK(contains(listing.out, "tar : 6 fichiers, 1 dossier, 2 autres éléments"));

    TempDir out;
    const std::string args = "extract " + fixture("basic.7z") + " -o " + quote((out / "x").string());
    CHECK(run(args, "fr").exit_code == 0);
    const Result again = run(args, "fr");
    CHECK(again.exit_code == 1);
    CHECK(contains(again.out, "Extrait : 0 fichier, "));
    CHECK(contains(again.err, "avertissement : hello.txt : existe déjà (already exists, skipped)"));

    // Scripts get the same JSON in every language.
    const Result json = run("list --json " + fixture("basic.tar"), "fr");
    CHECK(json.out == run("list --json " + fixture("basic.tar"), "en").out);
}

TEST_CASE("cli: usage errors exit with 2")
{
    const std::string args = GENERATE(std::string("frobnicate x.zip"), std::string("list"),
                                      std::string("list a.zip b.zip"), std::string("list --strip 1 a.zip"),
                                      std::string("extract a.zip --overwrite sometimes"),
                                      std::string("extract a.zip --max-size 12Q"), std::string("test a.zip --oops"));
    CAPTURE(args);
    const Result r = run(args);
    CHECK(r.exit_code == 2);
    CHECK(contains(r.err, "Try 'digzip --help'"));
}

TEST_CASE("cli: list as text and JSON")
{
    const Result text = run("list " + fixture("basic.tar"));
    CHECK(text.exit_code == 0);
    CHECK(contains(text.out, "hello.txt"));
    CHECK(contains(text.out, "links/hello-link -> ../hello.txt"));
    CHECK(contains(text.out, "tar: 6 files, 1 directory, 2 other entries"));

    const Result json = run("l " + fixture("basic.tar") + " --json --include \"*.md\"");
    CHECK(json.exit_code == 0);
    CHECK(json.out.rfind("{\"archive\":", 0) == 0);
    CHECK(contains(json.out, "\"path\":\"docs/readme.md\""));
    CHECK_FALSE(contains(json.out, "hello.txt"));
    CHECK(contains(json.out, "\"mode\":420"));

    const Result unicode = run("list " + fixture("basic.tar") + " --json");
    CHECK(contains(unicode.out, "caf\xC3\xA9 \xE2\x98\x95.txt"));

    const Result missing = run("list " + fixture("no-such-archive.zip") + " --json");
    CHECK(missing.exit_code == 3);
    CHECK(contains(missing.out, "\"ok\":false"));
}

TEST_CASE("cli: extract options and exit codes")
{
    TempDir dir;
    const std::string out = " -o " + quote((dir / "out").string());

    const Result first = run("extract " + fixture("data-descriptor.zip") + out);
    CHECK(first.exit_code == 0);
    CHECK(contains(first.out, "Extracted"));
    CHECK(fixtures::read_file(dir / "out/hello.txt") == fixtures::bytes("Hello, DigZip!\n"));

    const Result again = run("x " + fixture("data-descriptor.zip") + out);
    CHECK(again.exit_code == 1);
    CHECK(contains(again.err, "warning: hello.txt: already exists, skipped"));

    const Result renamed = run("x " + fixture("data-descriptor.zip") + out + " --overwrite rename --include hello.txt");
    CHECK(renamed.exit_code == 1);
    CHECK(fs::exists(dir / "out/hello (1).txt"));

    const Result stripped =
        run("x " + fixture("basic.tar") + " -o " + quote((dir / "s").string()) + " --strip 1 --include docs --quiet");
    CHECK(stripped.exit_code == 0);
    CHECK(stripped.out.empty());
    CHECK(fs::exists(dir / "s/readme.md"));

    const Result json = run("x " + fixture("data-descriptor.zip") + " -o " + quote((dir / "j").string()) + " --json");
    CHECK(json.exit_code == 0);
    CHECK(contains(json.out, "\"action\":\"extract\""));
    CHECK(contains(json.out, "\"ok\":true"));
}

TEST_CASE("cli: exit codes by kind of problem")
{
    const auto [args, code] = GENERATE(table<std::string, int>({
        {"test " + fixture("basic.7z"), 0},
        {"test " + fixture("evil-traversal.tar"), 5},
        {"test " + fixture("bomb.gz") + " --max-ratio 10 --max-size 1M", 5},
        {"test " + fixture("bad-crc.gz"), 4},
        {"test " + fixture("garbage.bin"), 4},
        {"test " + fixture("encrypted.7z"), 6},
        {"test " + fixture("encrypted.7z") + " -p nope", 6},
        {"test " + fixture("encrypted.7z") + " -p nope -p secret", 0},
        {"test " + fixture("no-such-archive.7z"), 3},
    }));
    CAPTURE(args);
    CHECK(run(args).exit_code == code);
}

TEST_CASE("cli: passwords from a file and from the environment")
{
    TempDir dir;
    // UTF-8 with a byte order mark and Windows line endings.
    fixtures::write_file(dir / "passwords.txt", "\xEF\xBB\xBFwrong\r\np\xC3\xA4ssw\xC3\xB6rd \xE2\x98\x95\r\n");
    const Result from_file = run("test " + fixture("encrypted-unicode.7z") + " --password-file " +
                                 quote((dir / "passwords.txt").string()));
    CHECK(from_file.exit_code == 0);

    set_env("DIGZIP_TEST_PASSWORD", "secret");
    CHECK(run("test " + fixture("aes256.zip") + " --password-env DIGZIP_TEST_PASSWORD").exit_code == 0);
    CHECK(run("test " + fixture("aes256.zip") + " --password-env DIGZIP_UNSET_VARIABLE").exit_code == 2);

    const Result wrong = run("test " + fixture("aes256.zip") + " -p Hunter2Hunter2 --json");
    CHECK(wrong.exit_code == 6);
    CHECK_FALSE(contains(wrong.out, "Hunter2Hunter2"));
    CHECK_FALSE(contains(wrong.err, "Hunter2Hunter2"));
}
