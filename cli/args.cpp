#include "args.hpp"
#include "i18n.hpp"
#include "platform.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>

namespace cli {

namespace fs = std::filesystem;

namespace {

std::uint64_t parse_number(const std::string& option, const std::string& text)
{
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos || text.size() > 19) {
        throw UsageError(fill(tr("{1}: expected a number, got '{2}'"), {option, text}));
    }
    return std::stoull(text);
}

// A whole number with an optional unit K, M, G or T (powers of 1024): "512", "64K", "2G".
std::uint64_t parse_size(const std::string& option, std::string text)
{
    std::uint64_t scale = 1;
    std::string suffix;
    while (!text.empty() && std::isalpha(static_cast<unsigned char>(text.back()))) {
        suffix.insert(suffix.begin(), static_cast<char>(std::toupper(static_cast<unsigned char>(text.back()))));
        text.pop_back();
    }
    if (suffix == "K" || suffix == "KB" || suffix == "KIB") {
        scale = 1ull << 10;
    } else if (suffix == "M" || suffix == "MB" || suffix == "MIB") {
        scale = 1ull << 20;
    } else if (suffix == "G" || suffix == "GB" || suffix == "GIB") {
        scale = 1ull << 30;
    } else if (suffix == "T" || suffix == "TB" || suffix == "TIB") {
        scale = 1ull << 40;
    } else if (!suffix.empty() && suffix != "B") {
        throw UsageError(fill(tr("{1}: unknown size unit '{2}'"), {option, suffix}));
    }
    const std::uint64_t value = parse_number(option, text);
    if (value > std::numeric_limits<std::uint64_t>::max() / scale) {
        throw UsageError(fill(tr("{1}: size too large"), {option}));
    }
    return value * scale;
}

// One password per line, UTF-8. Line endings and a byte order mark are removed.
void read_password_file(const std::string& file, std::vector<std::string>& passwords)
{
    std::ifstream in(path_from_utf8(file), std::ios::binary);
    if (!in) {
        throw UsageError(fill(tr("--password-file: cannot read '{1}'"), {file}));
    }
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (first && line.compare(0, 3, "\xEF\xBB\xBF") == 0) {
            line.erase(0, 3);
        }
        first = false;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            passwords.push_back(line);
        }
    }
}

Action action_of(const std::string& word)
{
    if (word == "list" || word == "l") {
        return Action::List;
    }
    if (word == "extract" || word == "x") {
        return Action::Extract;
    }
    if (word == "test" || word == "t") {
        return Action::Test;
    }
    throw UsageError(fill(tr("unknown command '{1}'"), {word}));
}

} // namespace

Command parse(const std::vector<std::string>& args)
{
    Command command;
    if (args.empty()) {
        return command;
    }
    if (args[0] == "-h" || args[0] == "--help" || args[0] == "help") {
        return command;
    }
    if (args[0] == "-V" || args[0] == "--version") {
        command.action = Action::Version;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] != "--json") {
                throw UsageError(fill(tr("unexpected argument '{1}'"), {args[i]}));
            }
            command.json = true;
        }
        return command;
    }
    command.action = action_of(args[0]);

    bool have_archive = false;
    digzip::ExtractOptions& o = command.options;
    for (std::size_t i = 1; i < args.size(); ++i) {
        std::string arg = args[i];
        std::string value;
        bool has_inline_value = false;
        if (arg.rfind("--", 0) == 0) {
            const std::size_t equals = arg.find('=');
            if (equals != std::string::npos) {
                value = arg.substr(equals + 1);
                arg.erase(equals);
                has_inline_value = true;
            }
        }
        const auto next = [&]() -> std::string {
            if (has_inline_value) {
                return value;
            }
            if (i + 1 >= args.size()) {
                throw UsageError(fill(tr("{1}: missing value"), {arg}));
            }
            return args[++i];
        };
        const bool extracting = command.action == Action::Extract;
        const auto extract_only = [&] {
            if (!extracting) {
                throw UsageError(fill(tr("{1} is only valid with 'extract'"), {arg}));
            }
        };

        if (arg == "-o" || arg == "--output") {
            extract_only();
            command.output = path_from_utf8(next());
        } else if (arg == "--include") {
            o.include.push_back(next());
        } else if (arg == "--exclude") {
            o.exclude.push_back(next());
        } else if (arg == "--strip") {
            extract_only();
            o.strip_components = static_cast<unsigned>(std::min<std::uint64_t>(parse_number(arg, next()), 4096));
        } else if (arg == "--overwrite") {
            extract_only();
            const std::string policy = next();
            if (policy == "skip") {
                o.overwrite = digzip::Overwrite::Skip;
            } else if (policy == "replace") {
                o.overwrite = digzip::Overwrite::Replace;
            } else if (policy == "rename") {
                o.overwrite = digzip::Overwrite::Rename;
            } else {
                throw UsageError(tr("--overwrite: expected skip, replace or rename"));
            }
        } else if (arg == "-p" || arg == "--password") {
            o.passwords.push_back(next());
        } else if (arg == "--password-file") {
            read_password_file(next(), o.passwords);
        } else if (arg == "--password-env") {
            const std::string name = next();
            const auto password = environment(name);
            if (!password) {
                throw UsageError(fill(tr("--password-env: the environment variable {1} is not set"), {name}));
            }
            o.passwords.push_back(*password);
        } else if (arg == "--max-size") {
            o.limits.max_total_size = parse_size(arg, next());
        } else if (arg == "--max-entries") {
            o.limits.max_entries = parse_number(arg, next());
        } else if (arg == "--max-ratio") {
            o.limits.max_ratio = parse_number(arg, next());
        } else if (arg == "--max-depth") {
            o.limits.max_path_depth = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(parse_number(arg, next()), std::numeric_limits<std::uint32_t>::max()));
        } else if (arg == "--max-memory") {
            o.limits.max_memory = parse_size(arg, next());
        } else if (arg == "--no-limits") {
            o.limits = digzip::Limits{0, 0, 0, 0, 0, 0};
        } else if (arg == "--json") {
            command.json = true;
        } else if (arg == "-q" || arg == "--quiet") {
            command.quiet = true;
        } else if (arg == "--no-progress") {
            command.progress = false;
        } else if (!arg.empty() && arg[0] == '-' && arg != "-") {
            throw UsageError(fill(tr("unknown option '{1}'"), {arg}));
        } else if (!have_archive) {
            command.archive = path_from_utf8(arg);
            have_archive = true;
        } else {
            throw UsageError(fill(tr("unexpected argument '{1}' (one archive at a time)"), {arg}));
        }
    }
    if (!have_archive) {
        throw UsageError(tr("missing archive"));
    }
    return command;
}

const char* help_text()
{
    static const char* const english = R"(DigZip - fast, safe archive extractor

Usage:
  digzip list    <archive> [options]     List the entries (alias: l)
  digzip extract <archive> [options]     Extract (alias: x)
  digzip test    <archive> [options]     Decode everything and verify checksums (alias: t)
  digzip --version [--json]
  digzip --help

Formats: zip, 7z, rar (RAR 5), tar, tar.gz/bz2/xz/zst, gz, bz2, xz, zst.

Selection:
  --include PATTERN        Only entries matching PATTERN ('*' and '?'; repeatable).
                           A pattern matching a directory selects its content.
  --exclude PATTERN        Skip entries matching PATTERN (repeatable).

Extraction:
  -o, --output DIR         Destination directory (default: current directory).
  --strip N                Remove N leading directories from every path.
  --overwrite POLICY       When a file exists: skip (default), replace, or rename.

Passwords (tried in order):
  -p, --password TEXT      A password (visible in the shell history; prefer the next two).
  --password-file FILE     One password per line.
  --password-env NAME      Read a password from an environment variable.

Safety limits (0 disables one):
  --max-size SIZE          Total uncompressed size (default 64G; units K, M, G, T).
  --max-entries N          Number of entries (default 1000000).
  --max-ratio N            Uncompressed / archive size (default 1000).
  --max-depth N            Directory levels in a path (default 256).
  --max-memory SIZE        Memory per decoder (default 1G).
  --no-limits              Disable every limit.

Output:
  --json                   Machine-readable JSON on standard output (always in English).
  -q, --quiet              Only report errors.
  --no-progress            No progress bar.

Language: the system's, or set DIGZIP_LANG to "en" or "fr".

Exit codes:
  0  success                    4  corrupt or unsupported data
  1  success with warnings      5  entries refused (security) or limit reached
  2  command line error         6  password missing or wrong
  3  input/output error         130  interrupted (Ctrl+C)
)";
    static const char* const french = R"(DigZip - extracteur d'archives rapide et sûr

Utilisation :
  digzip list    <archive> [options]     Lister le contenu (alias : l)
  digzip extract <archive> [options]     Extraire (alias : x)
  digzip test    <archive> [options]     Tout décoder et vérifier les sommes de contrôle (alias : t)
  digzip --version [--json]
  digzip --help

Formats : zip, 7z, rar (RAR 5), tar, tar.gz/bz2/xz/zst, gz, bz2, xz, zst.

Sélection :
  --include MOTIF          Seulement les éléments qui correspondent à MOTIF ('*' et '?' ;
                           répétable). Un motif qui désigne un dossier sélectionne son contenu.
  --exclude MOTIF          Ignorer les éléments qui correspondent à MOTIF (répétable).

Extraction :
  -o, --output DOSSIER     Dossier de destination (par défaut : le dossier courant).
  --strip N                Retirer les N premiers dossiers de chaque chemin.
  --overwrite POLITIQUE    Si un fichier existe : skip (garder, par défaut), replace
                           (remplacer) ou rename (renommer le nouveau).

Mots de passe (essayés dans l'ordre) :
  -p, --password TEXTE     Un mot de passe (visible dans l'historique du shell ; préférez
                           les deux options suivantes).
  --password-file FICHIER  Un mot de passe par ligne.
  --password-env NOM       Lire un mot de passe dans une variable d'environnement.

Limites de sécurité (0 en désactive une) :
  --max-size TAILLE        Taille totale décompressée (64G par défaut ; unités K, M, G, T).
  --max-entries N          Nombre d'éléments (1000000 par défaut).
  --max-ratio N            Taille décompressée / taille de l'archive (1000 par défaut).
  --max-depth N            Niveaux de dossiers dans un chemin (256 par défaut).
  --max-memory TAILLE      Mémoire par décodeur (1G par défaut).
  --no-limits              Désactiver toutes les limites.

Sortie :
  --json                   JSON sur la sortie standard, pour les scripts (toujours en anglais).
  -q, --quiet              N'afficher que les erreurs.
  --no-progress            Pas de barre de progression.

Langue : celle du système, ou DIGZIP_LANG à « en » ou « fr ».

Codes de retour :
  0    succès
  1    succès avec avertissements
  2    erreur de ligne de commande
  3    erreur d'entrée/sortie
  4    données endommagées ou non prises en charge
  5    éléments refusés (sécurité) ou limite atteinte
  6    mot de passe absent ou incorrect
  130  interrompu (Ctrl+C)
)";
    return language() == Language::French ? french : english;
}

} // namespace cli
