#include "scb/core/DepfileParser.hpp"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string_view>

namespace scb {
namespace {

[[nodiscard]] std::string ReadTextFile(
    const std::filesystem::path& path,
    std::vector<Diagnostic>& diagnostics)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        diagnostics.push_back({
            DiagnosticSeverity::Warning,
            "failed to open depfile: " + path.string(),
            std::nullopt,
        });
        return {};
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    if (!stream && !stream.eof()) {
        diagnostics.push_back({
            DiagnosticSeverity::Warning,
            "failed to read depfile: " + path.string(),
            std::nullopt,
        });
        return {};
    }
    return buffer.str();
}

[[nodiscard]] std::filesystem::path NormalizeDependencyPath(
    const std::string& raw,
    const std::filesystem::path& workingDirectory)
{
    std::filesystem::path path(raw);
    if (path.is_absolute()) {
        return path.lexically_normal();
    }
    return (workingDirectory / path).lexically_normal();
}

void SkipUtf8Bom(std::string& text)
{
    if (text.size() >= 3 &&
        static_cast<std::uint8_t>(text[0]) == 0xEF &&
        static_cast<std::uint8_t>(text[1]) == 0xBB &&
        static_cast<std::uint8_t>(text[2]) == 0xBF) {
        text.erase(text.begin(), text.begin() + 3);
    }
}

[[nodiscard]] std::string UnescapeJsonString(std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char current = value[index];
        if (current == '\\' && index + 1 < value.size()) {
            const char next = value[index + 1];
            switch (next) {
            case '"':
                result.push_back('"');
                break;
            case '\\':
                result.push_back('\\');
                break;
            case '/':
                result.push_back('/');
                break;
            case 'b':
                result.push_back('\b');
                break;
            case 'f':
                result.push_back('\f');
                break;
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 't':
                result.push_back('\t');
                break;
            default:
                result.push_back(next);
                break;
            }
            ++index;
            continue;
        }
        result.push_back(current);
    }
    return result;
}

} // namespace

std::vector<std::filesystem::path> ParseGnuMakeDepfile(
    const std::filesystem::path& depfile,
    const std::filesystem::path& workingDirectory,
    std::vector<Diagnostic>& diagnostics)
{
    std::ifstream stream(depfile);
    if (!stream) {
        diagnostics.push_back({
            DiagnosticSeverity::Warning,
            "failed to open depfile: " + depfile.string(),
            std::nullopt,
        });
        return {};
    }

    std::string raw;
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!raw.empty()) {
            raw.push_back('\n');
        }
        raw += line;
    }

    if (raw.empty()) {
        diagnostics.push_back({
            DiagnosticSeverity::Warning,
            "depfile is empty: " + depfile.string(),
            std::nullopt,
        });
        return {};
    }

    std::string flattened;
    flattened.reserve(raw.size());
    for (std::size_t index = 0; index < raw.size(); ++index) {
        const char current = raw[index];
        if (current == '\\' && index + 1 < raw.size() && raw[index + 1] == '\n') {
            ++index;
            continue;
        }
        if (current == '\n') {
            flattened.push_back(' ');
            continue;
        }
        flattened.push_back(current);
    }

    std::size_t colon = std::string::npos;
    bool escaped = false;
    for (std::size_t index = 0; index < flattened.size(); ++index) {
        const char current = flattened[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (current == '\\') {
            escaped = true;
            continue;
        }
        if (current == ':') {
            colon = index;
            break;
        }
    }

    if (colon == std::string::npos) {
        diagnostics.push_back({
            DiagnosticSeverity::Warning,
            "depfile is missing target separator: " + depfile.string(),
            std::nullopt,
        });
        return {};
    }

    std::vector<std::filesystem::path> dependencies;
    std::string token;
    escaped = false;
    for (std::size_t index = colon + 1; index < flattened.size(); ++index) {
        const char current = flattened[index];
        if (escaped) {
            token.push_back(current);
            escaped = false;
            continue;
        }
        if (current == '\\') {
            escaped = true;
            continue;
        }
        if (current == ' ' || current == '\t') {
            if (!token.empty()) {
                dependencies.push_back(NormalizeDependencyPath(token, workingDirectory));
                token.clear();
            }
            continue;
        }
        token.push_back(current);
    }
    if (!token.empty()) {
        dependencies.push_back(NormalizeDependencyPath(token, workingDirectory));
    }

    return dependencies;
}

std::vector<std::filesystem::path> ParseSourceDependenciesFile(
    const std::filesystem::path& depfile,
    const std::filesystem::path& workingDirectory,
    std::vector<Diagnostic>& diagnostics)
{
    auto text = ReadTextFile(depfile, diagnostics);
    if (text.empty()) {
        if (std::filesystem::exists(depfile) && std::filesystem::file_size(depfile) == 0) {
            diagnostics.push_back({
                DiagnosticSeverity::Warning,
                "depfile is empty: " + depfile.string(),
                std::nullopt,
            });
        }
        return {};
    }

    SkipUtf8Bom(text);

    const auto keyPosition = text.find("\"Includes\"");
    if (keyPosition == std::string::npos) {
        // Source file with no includes still produces an empty Includes array,
        // but treat a missing key as an empty list rather than an error.
        return {};
    }

    const auto arrayStart = text.find('[', keyPosition);
    if (arrayStart == std::string::npos) {
        diagnostics.push_back({
            DiagnosticSeverity::Warning,
            "depfile Includes key has no array: " + depfile.string(),
            std::nullopt,
        });
        return {};
    }

    std::vector<std::filesystem::path> dependencies;
    std::size_t index = arrayStart + 1;
    while (index < text.size()) {
        // Skip whitespace and commas between elements.
        while (index < text.size() && (text[index] == ' ' || text[index] == '\t' ||
                                           text[index] == '\n' || text[index] == '\r' ||
                                           text[index] == ',')) {
            ++index;
        }
        if (index >= text.size()) {
            break;
        }
        if (text[index] == ']') {
            break;
        }
        if (text[index] != '"') {
            diagnostics.push_back({
                DiagnosticSeverity::Warning,
                "depfile Includes array has malformed element: " + depfile.string(),
                std::nullopt,
            });
            return {};
        }

        ++index; // skip opening quote
        const auto stringStart = index;
        bool escaped = false;
        while (index < text.size()) {
            const char current = text[index];
            if (escaped) {
                escaped = false;
                ++index;
                continue;
            }
            if (current == '\\') {
                escaped = true;
                ++index;
                continue;
            }
            if (current == '"') {
                break;
            }
            ++index;
        }

        if (index >= text.size()) {
            diagnostics.push_back({
                DiagnosticSeverity::Warning,
                "depfile Includes array has unterminated string: " + depfile.string(),
                std::nullopt,
            });
            return {};
        }

        const std::string raw(text, stringStart, index - stringStart);
        const auto unescaped = UnescapeJsonString(raw);
        dependencies.push_back(NormalizeDependencyPath(unescaped, workingDirectory));

        ++index; // skip closing quote
        // Skip trailing whitespace.
        while (index < text.size() && (text[index] == ' ' || text[index] == '\t' ||
                                           text[index] == '\n' || text[index] == '\r')) {
            ++index;
        }
        if (index < text.size() && text[index] == ',') {
            ++index;
        }
    }

    return dependencies;
}

} // namespace scb
