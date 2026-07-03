#include "scb/core/BuildPlan.hpp"
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

void PrintUsage()
{
    std::cerr << "usage: scb build [--release] [--manifest-path <path>] [--dry-run] [--verbose]\n";
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

int BuildCommand(int argc, char** argv)
{
    std::optional<std::filesystem::path> manifestPath;
    bool release = false;
    bool dryRun = false;
    bool verbose = false;
    std::optional<std::string> compilerOverride;
    std::filesystem::path projectRoot;
    scb::ProjectManifest manifest;
    bool hasManifest = false;
    std::string manifestSummary = "zero-config";

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
        if (argument == "--manifest-path") {
            if (index + 1 >= argc) {
                PrintUsage();
                return 2;
            }
            manifestPath = std::filesystem::path(argv[++index]);
            continue;
        }

        PrintUsage();
        return 2;
    }

    if (manifestPath.has_value()) {
        const auto absoluteManifest = std::filesystem::absolute(*manifestPath).lexically_normal();
        if (!std::filesystem::exists(absoluteManifest)) {
            PrintDiagnostics({{
                scb::DiagnosticSeverity::Error,
                "manifest file does not exist: " + absoluteManifest.string(),
                scb::Diagnostic::Location{absoluteManifest, 0, 0},
            }});
            return 1;
        }

        const auto parsed = scb::ParseManifestFile({absoluteManifest});
        if (!parsed.ok()) {
            PrintDiagnostics(parsed.diagnostics);
            return 1;
        }

        projectRoot = parsed.projectRoot;
        manifest = parsed.manifest;
        hasManifest = true;
        compilerOverride = scb::GetCompilerOverride(manifest);
        manifestSummary = absoluteManifest.string();
    } else {
        const auto currentRoot = std::filesystem::current_path().lexically_normal();
        const auto discoveredManifest = currentRoot / "scb.toml";
        if (std::filesystem::exists(discoveredManifest)) {
            const auto parsed = scb::ParseManifestFile({discoveredManifest});
            if (!parsed.ok()) {
                PrintDiagnostics(parsed.diagnostics);
                return 1;
            }

            projectRoot = parsed.projectRoot;
            manifest = parsed.manifest;
            hasManifest = true;
            compilerOverride = scb::GetCompilerOverride(manifest);
            manifestSummary = discoveredManifest.string();
        } else {
            projectRoot = currentRoot;
            hasManifest = false;
        }
    }

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

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        PrintUsage();
        return 2;
    }

    if (std::string_view(argv[1]) == "build") {
        return BuildCommand(argc, argv);
    }

    PrintUsage();
    return 2;
}
