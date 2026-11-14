#include "i18n.hpp"
#include "platform.hpp"

#include <string_view>
#include <unordered_map>

namespace cli {

namespace {

Language g_language = Language::English;

const std::unordered_map<std::string_view, const char*>& french()
{
    static const std::unordered_map<std::string_view, const char*> table = {
        // Sizes
        {"B", "o"},
        {"KiB", "Kio"},
        {"MiB", "Mio"},
        {"GiB", "Gio"},
        {"TiB", "Tio"},
        // Listing
        {"Modified (UTC)", "Modifié (UTC)"},
        {"Type", "Type"},
        {"Size", "Taille"},
        {"Name", "Nom"},
        {"file", "fichier"},
        {"files", "fichiers"},
        {"directory", "dossier"},
        {"directories", "dossiers"},
        {"symlink", "lien"},
        {"hardlink", "lien dur"},
        {"special", "spécial"},
        {"link", "lien"},
        {"links", "liens"},
        {"entry", "élément"},
        {"entries", "éléments"},
        {"other entry", "autre élément"},
        {"other entries", "autres éléments"},
        {"entry skipped", "élément ignoré"},
        {"entries skipped", "éléments ignorés"},
        {"[encrypted]", "[chiffré]"},
        {"{1}: {2}, {3}, {4}, {5}", "{1} : {2}, {3}, {4}, {5}"},
        // Reports
        {"error: ", "erreur : "},
        {"warning: ", "avertissement : "},
        {"{1}: {2}", "{1} : {2}"},
        {"{1} ({2})", "{1} ({2})"},
        {"Extracted {1}, {2}, {3} ({4})", "Extrait : {1}, {2}, {3} ({4})"},
        {"Tested {1}, {2}, {3} ({4})", "Vérifié : {1}, {2}, {3} ({4})"},
        {" - with errors", " - avec des erreurs"},
        // Problem kinds, before the engine's own message
        {"input/output error", "erreur d'entrée/sortie"},
        {"damaged data", "données endommagées"},
        {"not supported", "non pris en charge"},
        {"checksum mismatch", "somme de contrôle incorrecte"},
        {"refused for safety", "refusé par sécurité"},
        {"safety limit reached", "limite de sécurité atteinte"},
        {"invalid name", "nom invalide"},
        {"already exists", "existe déjà"},
        {"invalid option", "option invalide"},
        {"cancelled", "annulé"},
        {"password required", "mot de passe requis"},
        {"wrong password", "mot de passe incorrect"},
        // Command line
        {"Try 'digzip --help'.", "Essayez « digzip --help »."},
        {"{1}: expected a number, got '{2}'", "{1} : nombre attendu, reçu « {2} »"},
        {"{1}: unknown size unit '{2}'", "{1} : unité de taille inconnue « {2} »"},
        {"{1}: size too large", "{1} : taille trop grande"},
        {"--password-file: cannot read '{1}'", "--password-file : impossible de lire « {1} »"},
        {"unknown command '{1}'", "commande inconnue « {1} »"},
        {"unexpected argument '{1}'", "argument inattendu « {1} »"},
        {"{1}: missing value", "{1} : valeur manquante"},
        {"{1} is only valid with 'extract'", "{1} n'est valable qu'avec « extract »"},
        {"--overwrite: expected skip, replace or rename", "--overwrite : skip, replace ou rename attendu"},
        {"--password-env: the environment variable {1} is not set",
         "--password-env : la variable d'environnement {1} n'est pas définie"},
        {"unknown option '{1}'", "option inconnue « {1} »"},
        {"unexpected argument '{1}' (one archive at a time)", "argument inattendu « {1} » (une archive à la fois)"},
        {"missing archive", "archive manquante"},
    };
    return table;
}

const char* label(digzip::ErrorCode code)
{
    using digzip::ErrorCode;
    switch (code) {
    case ErrorCode::Io: return "input/output error";
    case ErrorCode::Corrupt: return "damaged data";
    case ErrorCode::Unsupported: return "not supported";
    case ErrorCode::Checksum: return "checksum mismatch";
    case ErrorCode::Security: return "refused for safety";
    case ErrorCode::LimitExceeded: return "safety limit reached";
    case ErrorCode::InvalidName: return "invalid name";
    case ErrorCode::AlreadyExists: return "already exists";
    case ErrorCode::InvalidArgument: return "invalid option";
    case ErrorCode::Cancelled: return "cancelled";
    case ErrorCode::PasswordRequired: return "password required";
    case ErrorCode::WrongPassword: return "wrong password";
    }
    return "input/output error";
}

std::size_t characters(const std::string& text)
{
    std::size_t count = 0;
    for (const char c : text) {
        count += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
    }
    return count;
}

Language from_code(const std::string& code)
{
    return code.compare(0, 2, "fr") == 0 ? Language::French : Language::English;
}

} // namespace

Language detect_language()
{
    if (const auto forced = environment("DIGZIP_LANG"); forced && !forced->empty()) {
        return from_code(*forced);
    }
    return from_code(system_language());
}

void set_language(Language language)
{
    g_language = language;
}

Language language()
{
    return g_language;
}

const char* tr(const char* english)
{
    if (g_language == Language::French) {
        const auto& table = french();
        if (const auto it = table.find(english); it != table.end()) {
            return it->second;
        }
    }
    return english;
}

std::string fill(std::string text, std::initializer_list<std::string> args)
{
    std::size_t n = 1;
    for (const std::string& arg : args) {
        const std::string marker = "{" + std::to_string(n++) + "}";
        if (const std::size_t at = text.find(marker); at != std::string::npos) {
            text.replace(at, marker.size(), arg);
        }
    }
    return text;
}

std::string counted(std::uint64_t count, const char* one, const char* many)
{
    // French uses the singular for 0 and 1, English only for 1.
    const bool singular = g_language == Language::French ? count <= 1 : count == 1;
    return std::to_string(count) + " " + tr(singular ? one : many);
}

std::string describe(digzip::ErrorCode code, const std::string& message)
{
    if (g_language == Language::English) {
        return message;
    }
    return fill(tr("{1} ({2})"), {tr(label(code)), message});
}

std::string pad_right(const std::string& text, std::size_t width)
{
    const std::size_t length = characters(text);
    return length >= width ? text : text + std::string(width - length, ' ');
}

std::string pad_left(const std::string& text, std::size_t width)
{
    const std::size_t length = characters(text);
    return length >= width ? text : std::string(width - length, ' ') + text;
}

} // namespace cli
