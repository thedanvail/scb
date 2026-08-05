#include "scb/core/Toolchain.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <sstream>
#include <system_error>
#include <string_view>
#include <vector>

namespace scb {
namespace {

[[nodiscard]] bool HasErrors(const std::vector<Diagnostic>& diagnostics)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::Error;
    });
}

[[nodiscard]] std::string Lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

[[nodiscard]] ToolchainFamily InferFamily(const std::filesystem::path& compilerPath)
{
    const auto filename = Lowercase(compilerPath.filename().string());
    if (filename == "cl.exe" || filename == "cl") {
        return ToolchainFamily::Msvc;
    }
    if (filename == "clang++" || filename == "clang++.exe" || filename == "clang-cl.exe" || filename == "clang-cl") {
        return ToolchainFamily::Clang;
    }
    if (filename == "g++" || filename == "g++.exe") {
        return ToolchainFamily::Gcc;
    }
    return ToolchainFamily::Unknown;
}

[[nodiscard]] std::vector<std::filesystem::path> PathEntries()
{
    std::vector<std::filesystem::path> entries;
    const char* path = std::getenv("PATH");
    if (path == nullptr) {
        return entries;
    }

#ifdef _WIN32
    constexpr char separator = ';';
#else
    constexpr char separator = ':';
#endif

    std::stringstream stream(path);
    std::string entry;
    while (std::getline(stream, entry, separator)) {
        if (!entry.empty()) {
            entries.emplace_back(entry);
        }
    }
    return entries;
}

[[nodiscard]] std::optional<std::filesystem::path> FindExecutable(const std::string& executable)
{
    const std::filesystem::path candidate(executable);
    if (candidate.is_absolute() || candidate.has_parent_path()) {
        if (std::filesystem::exists(candidate)) {
            return std::filesystem::absolute(candidate).lexically_normal();
        }
        return std::nullopt;
    }

    for (const auto& entry : PathEntries()) {
        const auto path = (entry / candidate).lexically_normal();
        if (std::filesystem::exists(path) && !std::filesystem::is_directory(path)) {
            return path;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::filesystem::path> DetectArchiver(ToolchainFamily family, const std::filesystem::path& compilerPath)
{
    if (family == ToolchainFamily::Msvc) {
        const auto sibling = compilerPath.parent_path() / "lib.exe";
        if (std::filesystem::exists(sibling)) {
            return sibling.lexically_normal();
        }
        if (auto libExe = FindExecutable("lib.exe")) {
            return libExe;
        }
        return FindExecutable("lib");
    }

    if (family == ToolchainFamily::Clang) {
        if (auto llvmAr = FindExecutable("llvm-ar")) {
            return llvmAr;
        }
    }
    return FindExecutable("ar");
}

[[nodiscard]] std::optional<std::filesystem::path> DetectLinker(ToolchainFamily family, const std::filesystem::path& compilerPath)
{
    if (family == ToolchainFamily::Msvc) {
        const auto sibling = compilerPath.parent_path() / "link.exe";
        if (std::filesystem::exists(sibling)) {
            return sibling.lexically_normal();
        }
        if (auto linkExe = FindExecutable("link.exe")) {
            return linkExe;
        }
        return FindExecutable("link");
    }
    return compilerPath;
}

[[nodiscard]] std::optional<std::string> RunCommandAndCapture(const std::string& command)
{
#ifdef _WIN32
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (pipe == nullptr) {
        return std::nullopt;
    }

    std::string output;
    char buffer[4096];
    while (std::fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr) {
        output.append(buffer);
    }

#ifdef _WIN32
    const int exitCode = _pclose(pipe);
#else
    const int exitCode = pclose(pipe);
#endif
    if (exitCode != 0 || output.empty()) {
        return std::nullopt;
    }
    return output;
}

[[nodiscard]] std::string Trim(std::string value)
{
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    std::size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) {
        ++start;
    }
    return value.substr(start);
}

[[nodiscard]] std::string QuoteShellPath(const std::filesystem::path& path)
{
    std::string quoted = "\"";
    for (const char character : path.string()) {
        if (character == '"' || character == '\\') {
            quoted.push_back('\\');
        }
        quoted.push_back(character);
    }
    quoted.push_back('"');
    return quoted;
}

[[nodiscard]] std::string FirstNonEmptyLine(const std::string& text)
{
    std::stringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        line = Trim(line);
        if (!line.empty()) {
            return line;
        }
    }
    return {};
}

[[nodiscard]] std::string StableHashHex(std::string_view input)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char character : input) {
        hash ^= character;
        hash *= 1099511628211ull;
    }

    std::ostringstream stream;
    stream << std::hex << hash;
    return stream.str();
}

[[nodiscard]] std::string FallbackIdentityPayload(const std::filesystem::path& compilerPath)
{
    std::error_code error;
    const auto writeTime = std::filesystem::last_write_time(compilerPath, error);
    std::ostringstream stream;
    stream << compilerPath.lexically_normal().string();
    if (!error) {
        stream << '\n' << writeTime.time_since_epoch().count();
    }
    return stream.str();
}

void PopulateToolchainIdentity(ToolchainInfo& toolchain)
{
    const std::filesystem::path compilerPath(toolchain.compilerPath);
    const auto quotedPath = QuoteShellPath(compilerPath);

    std::optional<std::string> probeOutput;
    if (toolchain.family == ToolchainFamily::Msvc) {
        probeOutput = RunCommandAndCapture(quotedPath + " /Bv 2>&1");
        if (!probeOutput.has_value()) {
            probeOutput = RunCommandAndCapture(quotedPath + " /? 2>&1");
        }
    } else {
        probeOutput = RunCommandAndCapture(quotedPath + " --version 2>&1");
    }

    if (probeOutput.has_value()) {
        toolchain.version = FirstNonEmptyLine(*probeOutput);
        toolchain.identity = StableHashHex(compilerPath.lexically_normal().string() + "\n" + *probeOutput);
        return;
    }

    if (toolchain.version.empty()) {
        toolchain.version = compilerPath.filename().string();
    }
    toolchain.identity = StableHashHex(FallbackIdentityPayload(compilerPath));
}

} // namespace

bool DetectToolchainResult::ok() const
{
    return !HasErrors(diagnostics);
}

DetectToolchainResult DetectHostToolchain(const DetectToolchainRequest& request)
{
    DetectToolchainResult result;

    std::optional<std::filesystem::path> compilerPath;
    if (request.compilerOverride.has_value()) {
        std::filesystem::path overridePath(*request.compilerOverride);
        if (overridePath.is_relative() && overridePath.has_parent_path()) {
            overridePath = request.projectRoot / overridePath;
        }
        compilerPath = FindExecutable(overridePath.string());
        if (!compilerPath.has_value()) {
            result.diagnostics.push_back({
                DiagnosticSeverity::Error,
                "configured compiler does not exist: " + overridePath.string(),
                std::nullopt,
            });
            return result;
        }
    } else {
        for (const auto& candidate : {std::string("cl.exe"), std::string("cl"), std::string("g++"), std::string("clang++")}) {
            compilerPath = FindExecutable(candidate);
            if (compilerPath.has_value()) {
                break;
            }
        }
        if (!compilerPath.has_value()) {
            result.diagnostics.push_back({
                DiagnosticSeverity::Error,
                "no supported C++ compiler found in PATH",
                std::nullopt,
            });
            return result;
        }
    }

    result.toolchain.compilerPath = compilerPath->string();
    result.toolchain.family = InferFamily(*compilerPath);
    if (result.toolchain.family == ToolchainFamily::Unknown) {
        result.diagnostics.push_back({
            DiagnosticSeverity::Error,
            "unable to determine compiler family for: " + compilerPath->string(),
            std::nullopt,
        });
        return result;
    }

    if (auto archiver = DetectArchiver(result.toolchain.family, *compilerPath)) {
        result.toolchain.archiverPath = archiver->string();
    }
    if (auto linker = DetectLinker(result.toolchain.family, *compilerPath)) {
        result.toolchain.linkerPath = linker->string();
    }
    PopulateToolchainIdentity(result.toolchain);

    return result;
}

} // namespace scb
