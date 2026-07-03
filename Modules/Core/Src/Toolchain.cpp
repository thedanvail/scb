#include "scb/core/Toolchain.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <sstream>
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
    result.toolchain.version = compilerPath->filename().string();

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

    return result;
}

} // namespace scb
