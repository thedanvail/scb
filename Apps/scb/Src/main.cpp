#include "scb/core/BuildPlan.hpp"
#include "scb/core/CleanProject.hpp"
#include "scb/core/ExecuteBuild.hpp"
#include "scb/core/ManifestParser.hpp"
#include "scb/core/ResolvedProject.hpp"
#include "scb/core/Toolchain.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace {

constexpr const char* kVersion = "scb 0.1.0";

enum class ManifestLookupStatus {
    Found,
    NotFound,
    Error
};

struct ManifestLookupResult {
    ManifestLookupStatus status = ManifestLookupStatus::NotFound;
    std::optional<std::filesystem::path> manifestPath;
};

void PrintUsage()
{
    std::cerr << "usage: scb [--version] [--help] <command> [<args>]\n"
              << "\n"
              << "Commands:\n"
              << "  build     Compile the project\n"
              << "  clean     Remove build artifacts\n"
              << "\n"
              << "Run `scb <command> --help` for command-specific usage.\n";
}

void PrintBuildUsage()
{
    std::cerr << "usage: scb build [--release] [--manifest-path <path>] [--plan[=json]] [--dry-run] [--verbose]\n";
}

void PrintCleanUsage()
{
    std::cerr << "usage: scb clean [--all] [--profile <name>] [--manifest-path <path>]\n";
}

void PrintCommand(std::ostream& stream, const scb::CommandLine& command)
{
    stream << command.program;
    for (const auto& arg : command.args) {
        stream << ' ' << arg;
    }
    stream << '\n';
}

void PrintDiagnostics(const std::vector<scb::Diagnostic>& diagnostics)
{
    for (const auto& diagnostic : diagnostics) {
        std::cerr << scb::FormatDiagnostic(diagnostic) << '\n';
    }
}

[[nodiscard]] ManifestLookupResult FindManifestPath(
    std::optional<std::filesystem::path> manifestPath,
    std::filesystem::path& projectRoot,
    bool& hasManifest,
    scb::ProjectManifest& manifest,
    std::string& manifestSummary)
{
    if (manifestPath.has_value()) {
        const auto absoluteManifest = std::filesystem::absolute(*manifestPath).lexically_normal();
        if (!std::filesystem::exists(absoluteManifest)) {
            PrintDiagnostics({{
                scb::DiagnosticSeverity::Error,
                "manifest file does not exist: " + absoluteManifest.string(),
                scb::Diagnostic::Location{absoluteManifest, 0, 0},
            }});
            return {ManifestLookupStatus::Error, std::nullopt};
        }

        const auto parsed = scb::ParseManifestFile({absoluteManifest});
        if (!parsed.ok()) {
            PrintDiagnostics(parsed.diagnostics);
            return {ManifestLookupStatus::Error, std::nullopt};
        }

        projectRoot = parsed.projectRoot;
        manifest = parsed.manifest;
        hasManifest = true;
        manifestSummary = absoluteManifest.string();
        return {ManifestLookupStatus::Found, absoluteManifest};
    }

    const auto currentRoot = std::filesystem::current_path().lexically_normal();
    const auto discoveredManifest = currentRoot / "scb.toml";
    if (std::filesystem::exists(discoveredManifest)) {
        const auto parsed = scb::ParseManifestFile({discoveredManifest});
        if (!parsed.ok()) {
            PrintDiagnostics(parsed.diagnostics);
            return {ManifestLookupStatus::Error, std::nullopt};
        }

        projectRoot = parsed.projectRoot;
        manifest = parsed.manifest;
        hasManifest = true;
        manifestSummary = discoveredManifest.string();
        return {ManifestLookupStatus::Found, discoveredManifest};
    }

    projectRoot = currentRoot;
    hasManifest = false;
    manifestSummary = "zero-config";
    return {ManifestLookupStatus::NotFound, std::nullopt};
}

int BuildCommand(int argc, char** argv)
{
    std::optional<std::filesystem::path> manifestPath;
    bool release = false;
    bool dryRun = false;
    bool verbose = false;
    bool planOnly = false;
    std::string planFormat = "json";

    for (int index = 2; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--release") {
            release = true;
            continue;
        }
        if (argument == "--dry-run") {
            dryRun = true;
            continue;
        }
        if (argument == "--verbose") {
            verbose = true;
            continue;
        }
        if (argument == "--verbose-plan") {
            dryRun = true;
            verbose = true;
            continue;
        }
        if (argument == "--plan") {
            planOnly = true;
            planFormat = "json";
            continue;
        }
        if (argument.starts_with("--plan=")) {
            planOnly = true;
            planFormat = argument.substr(7);
            continue;
        }
        if (argument == "--manifest-path") {
            if (index + 1 >= argc) {
                PrintBuildUsage();
                return 2;
            }
            manifestPath = std::filesystem::path(argv[++index]);
            continue;
        }
        if (argument == "--help") {
            PrintBuildUsage();
            return 0;
        }

        PrintBuildUsage();
        return 2;
    }

    std::filesystem::path projectRoot;
    scb::ProjectManifest manifest;
    bool hasManifest = false;
    std::string manifestSummary = "zero-config";

    const bool explicitManifestPath = manifestPath.has_value();
    const auto manifestLookup = FindManifestPath(std::move(manifestPath), projectRoot, hasManifest, manifest, manifestSummary);
    if (manifestLookup.status == ManifestLookupStatus::Error) {
        return 1;
    }
    if (manifestLookup.status == ManifestLookupStatus::NotFound && !hasManifest) {
        projectRoot = std::filesystem::current_path().lexically_normal();
    }
    if (manifestLookup.status == ManifestLookupStatus::NotFound && explicitManifestPath) {
        return 1;
    }

    const auto compilerOverride = hasManifest ? scb::GetCompilerOverride(manifest) : std::nullopt;
    const auto detectedToolchain = scb::DetectHostToolchain({projectRoot, compilerOverride});
    if (!detectedToolchain.ok()) {
        PrintDiagnostics(detectedToolchain.diagnostics);
        return 1;
    }

    scb::ResolveRequest request;
    request.projectRoot = projectRoot;
    request.manifest = manifest;
    request.hasManifest = hasManifest;
    request.profile = release ? "release" : "debug";
    request.toolchain = detectedToolchain.toolchain;

    const auto resolved = scb::ResolveProject(request);
    if (!resolved.ok()) {
        PrintDiagnostics(resolved.diagnostics);
        return 1;
    }

    const auto plan = scb::PlanBuild({resolved.project});
    if (!plan.ok()) {
        PrintDiagnostics(plan.diagnostics);
        return 1;
    }

    if (planOnly) {
        if (planFormat.empty() || planFormat == "json") {
            std::cout << scb::ToJson(plan.plan);
        } else {
            PrintBuildUsage();
            return 2;
        }
        return 0;
    }

    const auto execution = scb::ExecuteBuild({plan.plan, dryRun, verbose});
    if (!execution.ok()) {
        for (std::size_t index = 0; index < execution.summary.actions.size(); ++index) {
            const auto& action = execution.summary.actions[index];
            const auto& planned = plan.plan.actions[index];
            if (action.status != scb::ActionStatus::Failed) {
                continue;
            }

            std::cerr << planned.label << '\n';
            PrintCommand(std::cerr, planned.command);
            if (!action.stdoutText.empty()) {
                std::cerr << action.stdoutText;
            }
            if (!action.stderrText.empty()) {
                std::cerr << action.stderrText;
            }
            break;
        }
        PrintDiagnostics(execution.diagnostics);
        return 1;
    }

    std::cout << "Project: " << resolved.project.name << '\n';
    std::cout << "Profile: " << resolved.project.profile.name << '\n';
    std::cout << "Manifest: " << manifestSummary << '\n';
    std::cout << "Toolchain: " << scb::ToString(plan.plan.project.toolchain.family)
              << " (" << plan.plan.project.toolchain.compilerPath << ")\n";

    for (std::size_t index = 0; index < execution.summary.actions.size(); ++index) {
        const auto& action = execution.summary.actions[index];
        const auto& planned = plan.plan.actions[index];

        if (dryRun) {
            std::cout << (action.status == scb::ActionStatus::Executed ? "Would execute " : "Would skip ")
                      << planned.label << '\n';
        } else if (action.status == scb::ActionStatus::Executed) {
            std::cout << planned.label << '\n';
        } else if (verbose) {
            std::cout << "Skip " << planned.label << '\n';
        }

        if (verbose) {
            if (!action.reason.empty()) {
                std::cout << "  reason: " << action.reason << '\n';
            }
            std::cout << "  cmd: ";
            PrintCommand(std::cout, planned.command);
            if (action.status == scb::ActionStatus::Executed && !action.stdoutText.empty()) {
                std::cout << "  stdout:\n" << action.stdoutText;
            }
            if (action.status == scb::ActionStatus::Executed && !action.stderrText.empty()) {
                std::cout << "  stderr:\n" << action.stderrText;
            }
        }
    }

    std::cout << "Finished " << resolved.project.profile.name
              << ": " << execution.summary.executed << (dryRun ? " would execute, " : " executed, ")
              << execution.summary.skipped << (dryRun ? " would skip" : " fresh") << '\n';
    return 0;
}

int CleanCommand(int argc, char** argv)
{
    std::optional<std::filesystem::path> manifestPath;
    std::string profile;
    bool allProfiles = false;

    for (int index = 2; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--all") {
            allProfiles = true;
            continue;
        }
        if (argument == "--profile") {
            if (index + 1 >= argc) {
                PrintCleanUsage();
                return 2;
            }
            profile = argv[++index];
            continue;
        }
        if (argument == "--manifest-path") {
            if (index + 1 >= argc) {
                PrintCleanUsage();
                return 2;
            }
            manifestPath = std::filesystem::path(argv[++index]);
            continue;
        }
        if (argument == "--help") {
            PrintCleanUsage();
            return 0;
        }

        PrintCleanUsage();
        return 2;
    }

    std::filesystem::path projectRoot;
    scb::ProjectManifest manifest;
    bool hasManifest = false;
    std::string manifestSummary = "zero-config";

    const auto manifestLookup = FindManifestPath(std::move(manifestPath), projectRoot, hasManifest, manifest, manifestSummary);
    if (manifestLookup.status == ManifestLookupStatus::Error) {
        return 1;
    }

    if (profile.empty() && !allProfiles && hasManifest && !manifest.profiles.empty()) {
        if (manifest.profiles.contains("debug")) {
            profile = "debug";
        } else {
            profile = manifest.profiles.begin()->first;
        }
    }

    if (profile.empty() && !allProfiles) {
        profile = "debug";
    }

    scb::CleanRequest request;
    request.projectRoot = projectRoot;
    request.profile = profile;
    request.allProfiles = allProfiles;

    const auto result = scb::CleanProject(request);
    if (!result.ok()) {
        PrintDiagnostics(result.diagnostics);
        return 1;
    }

    if (allProfiles) {
        std::cout << "Cleaned target/\n";
    } else if (!result.removedDirectories.empty()) {
        std::cout << "Cleaned " << result.removedDirectories.front().lexically_relative(projectRoot).string() << '\n';
    } else {
        std::cout << "Nothing to clean\n";
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        PrintUsage();
        return 2;
    }

    const std::string_view command(argv[1]);

    if (command == "--version" || command == "-V" || command == "--v") {
        std::cout << kVersion << '\n';
        return 0;
    }

    if (command == "--help" || command == "-h") {
        PrintUsage();
        return 0;
    }

    if (command == "build") {
        return BuildCommand(argc, argv);
    }

    if (command == "clean") {
        return CleanCommand(argc, argv);
    }

    std::cerr << "unknown command: " << command << "\n\n";
    PrintUsage();
    return 2;
}
