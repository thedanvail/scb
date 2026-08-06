#include "scb/core/BuildPlan.hpp"
#include "scb/core/CleanProject.hpp"
#include "scb/core/ExecuteBuild.hpp"
#include "scb/core/ManifestParser.hpp"
#include "scb/core/ResolvedProject.hpp"
#include "scb/core/Toolchain.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

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
              << "  test      Compile and run tests\n"
              << "  clean     Remove build artifacts\n"
              << "\n"
              << "Run `scb <command> --help` for command-specific usage.\n";
}

void PrintBuildUsage()
{
    std::cerr << "usage: scb build [--profile <name>] [--release] [--jobs <n>] [--manifest-path <path>] [--plan[=json]] [--dry-run] [--verbose]\n";
}

void PrintTestUsage()
{
    std::cerr << "usage: scb test [--profile <name>] [--release] [--jobs <n>] [--manifest-path <path>] [--dry-run] [--verbose]\n";
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

struct CommonCommandOptions {
    std::optional<std::filesystem::path> manifestPath;
    std::string profile = "debug";
    bool release = false;
    bool dryRun = false;
    bool verbose = false;
    std::size_t jobs = 1;
};

[[nodiscard]] bool ParsePositiveSize(std::string_view value, std::size_t& parsed)
{
    if (value.empty()) {
        return false;
    }
    std::size_t result = 0;
    for (const char character : value) {
        if (character < '0' || character > '9') {
            return false;
        }
        result = result * 10 + static_cast<std::size_t>(character - '0');
    }
    if (result == 0) {
        return false;
    }
    parsed = result;
    return true;
}

[[nodiscard]] std::optional<std::string> ParseCommonOption(
    int argc,
    char** argv,
    int& index,
    CommonCommandOptions& options)
{
    const std::string_view argument(argv[index]);
    if (argument == "--release") {
        if (options.profile != "debug") {
            return "cannot combine --release with --profile";
        }
        options.release = true;
        options.profile = "release";
        return std::string{};
    }
    if (argument == "--profile") {
        if (index + 1 >= argc) {
            return "missing value for --profile";
        }
        if (options.release) {
            return "cannot combine --release with --profile";
        }
        options.profile = argv[++index];
        return std::string{};
    }
    if (argument == "--dry-run") {
        options.dryRun = true;
        return std::string{};
    }
    if (argument == "--verbose") {
        options.verbose = true;
        return std::string{};
    }
    if (argument == "--verbose-plan") {
        options.dryRun = true;
        options.verbose = true;
        return std::string{};
    }
    if (argument == "--jobs") {
        if (index + 1 >= argc || !ParsePositiveSize(argv[index + 1], options.jobs)) {
            return "missing or invalid value for --jobs";
        }
        ++index;
        return std::string{};
    }
    if (argument == "--manifest-path") {
        if (index + 1 >= argc) {
            return "missing value for --manifest-path";
        }
        options.manifestPath = std::filesystem::path(argv[++index]);
        return std::string{};
    }
    return std::nullopt;
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

[[nodiscard]] bool IsCompileSource(const std::filesystem::path& path)
{
    const auto extension = path.extension().string();
    return extension == ".cpp" || extension == ".cc" || extension == ".cxx" || extension == ".c++";
}

[[nodiscard]] scb::ProjectPath MakeProjectPath(const std::filesystem::path& root, const std::filesystem::path& relative)
{
    const auto normalized = relative.lexically_normal();
    return {(root / normalized).lexically_normal(), normalized.generic_string()};
}

[[nodiscard]] std::string TestNameFromPath(const std::filesystem::path& relative)
{
    auto withoutExtension = relative;
    withoutExtension.replace_extension();
    std::string name = withoutExtension.generic_string();
    if (name.starts_with("tests/")) {
        name.erase(0, 6);
    }
    for (char& character : name) {
        if (character == '/') {
            character = '.';
        }
    }
    return name;
}

[[nodiscard]] std::optional<scb::TargetId> ProjectLibraryDependency(const scb::ResolvedProject& project)
{
    for (const auto kind : {scb::TargetKind::StaticLibrary, scb::TargetKind::SharedLibrary, scb::TargetKind::HeaderOnly}) {
        for (const auto& target : project.targets) {
            if (target.id.kind == kind && target.id.name == project.name) {
                return target.id;
            }
        }
    }
    return std::nullopt;
}

void AddDiscoveredTestTargets(scb::ResolvedProject& project)
{
    const auto testsDirectory = project.root.absolute / "tests";
    if (!std::filesystem::exists(testsDirectory) || !std::filesystem::is_directory(testsDirectory)) {
        return;
    }

    std::vector<std::filesystem::path> testSources;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(testsDirectory)) {
        if (entry.is_regular_file() && IsCompileSource(entry.path())) {
            testSources.push_back(entry.path().lexically_relative(project.root.absolute));
        }
    }
    std::sort(testSources.begin(), testSources.end());

    const auto libraryDependency = ProjectLibraryDependency(project);
    for (const auto& source : testSources) {
        scb::ResolvedTarget target;
        target.id = {scb::TargetKind::TestExecutable, TestNameFromPath(source)};
        target.origin = scb::TargetOrigin::Inferred;
        target.sources.compileSources.push_back({MakeProjectPath(project.root.absolute, source)});
        target.build = project.profile.build;
        target.artifactDirectory = MakeProjectPath(
            project.root.absolute,
            std::filesystem::path("target") / project.profile.name / "test" / target.id.name);
        if (libraryDependency.has_value()) {
            target.dependencies.push_back(*libraryDependency);
        }
        project.targets.push_back(std::move(target));
    }

    std::sort(project.targets.begin(), project.targets.end(), [](const scb::ResolvedTarget& lhs, const scb::ResolvedTarget& rhs) {
        return scb::ToString(lhs.id.kind) + ":" + lhs.id.name < scb::ToString(rhs.id.kind) + ":" + rhs.id.name;
    });
}

[[nodiscard]] std::vector<scb::ProjectPath> TestExecutables(const scb::BuildPlan& plan)
{
    std::vector<scb::ProjectPath> executables;
    for (const auto& action : plan.actions) {
        if (action.kind == scb::ActionKind::Link && action.owner.kind == scb::TargetKind::TestExecutable && !action.outputs.empty()) {
            executables.push_back(action.outputs.front().path);
        }
    }
    std::sort(executables.begin(), executables.end(), [](const scb::ProjectPath& lhs, const scb::ProjectPath& rhs) {
        return lhs.relative < rhs.relative;
    });
    return executables;
}

void PrepareTestRuntimeEnvironment(const scb::BuildPlan& plan)
{
#ifdef _WIN32
    std::set<std::string> directories;
    for (const auto& target : plan.project.targets) {
        if (target.id.kind == scb::TargetKind::SharedLibrary) {
            directories.insert(target.artifactDirectory.absolute.string());
        }
    }
    if (directories.empty()) {
        return;
    }

    std::string path;
    const char* existing = std::getenv("PATH");
    if (existing != nullptr) {
        path = existing;
    }
    for (const auto& directory : directories) {
        path = directory + ";" + path;
    }
    _putenv_s("PATH", path.c_str());
#else
    (void)plan;
#endif
}

[[nodiscard]] std::string QuoteShellPath(const std::filesystem::path& path)
{
#ifdef _WIN32
    std::string value = path.string();
    std::string quoted = "\"";
    for (const char character : value) {
        if (character == '"') {
            quoted += "\\\"";
        } else {
            quoted += character;
        }
    }
    quoted += "\"";
    return quoted;
#else
    std::string value = path.string();
    std::string quoted = "'";
    for (const char character : value) {
        if (character == '\'') {
            quoted += "'\\''";
        } else {
            quoted += character;
        }
    }
    quoted += "'";
    return quoted;
#endif
}

[[nodiscard]] int NormalizeSystemResult(int result)
{
    if (result == -1) {
        return 1;
    }
#ifndef _WIN32
    if (WIFEXITED(result)) {
        return WEXITSTATUS(result);
    }
    if (WIFSIGNALED(result)) {
        return 128 + WTERMSIG(result);
    }
#endif
    return result == 0 ? 0 : 1;
}

int BuildCommand(int argc, char** argv)
{
    CommonCommandOptions options;
    bool planOnly = false;
    std::string planFormat = "json";

    for (int index = 2; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        auto parsedCommon = ParseCommonOption(argc, argv, index, options);
        if (parsedCommon.has_value()) {
            if (!parsedCommon->empty()) {
                std::cerr << *parsedCommon << "\n";
                PrintBuildUsage();
                return 2;
            }
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

    const bool explicitManifestPath = options.manifestPath.has_value();
    const auto manifestLookup = FindManifestPath(std::move(options.manifestPath), projectRoot, hasManifest, manifest, manifestSummary);
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
    request.profile = options.profile;
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

    const auto execution = scb::ExecuteBuild({plan.plan, options.dryRun, options.verbose, options.jobs});
    if (!execution.ok()) {
        std::map<std::string, const scb::ActionNode*> actionsById;
        for (const auto& planned : plan.plan.actions) {
            actionsById[planned.id] = &planned;
        }
        for (const auto& action : execution.summary.actions) {
            if (action.status != scb::ActionStatus::Failed) {
                continue;
            }
            const auto plannedIt = actionsById.find(action.actionId);
            if (plannedIt == actionsById.end()) {
                continue;
            }
            const auto& planned = *plannedIt->second;

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

    std::map<std::string, const scb::ActionNode*> actionsById;
    for (const auto& action : plan.plan.actions) {
        actionsById[action.id] = &action;
    }

    std::cout << "Project: " << resolved.project.name << '\n';
    std::cout << "Profile: " << resolved.project.profile.name << '\n';
    std::cout << "Manifest: " << manifestSummary << '\n';
    std::cout << "Toolchain: " << scb::ToString(plan.plan.project.toolchain.family)
              << " (" << plan.plan.project.toolchain.compilerPath << ")\n";

    for (const auto& action : execution.summary.actions) {
        const auto plannedIt = actionsById.find(action.actionId);
        if (plannedIt == actionsById.end()) {
            continue;
        }
        const auto& planned = *plannedIt->second;

        if (options.dryRun) {
            std::cout << (action.status == scb::ActionStatus::Executed ? "Would execute " : "Would skip ")
                      << planned.label << '\n';
        } else if (action.status == scb::ActionStatus::Executed) {
            std::cout << planned.label << '\n';
        } else if (options.verbose) {
            std::cout << "Skip " << planned.label << '\n';
        }

        if (options.verbose) {
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
              << ": " << execution.summary.executed << (options.dryRun ? " would execute, " : " executed, ")
              << execution.summary.skipped << (options.dryRun ? " would skip" : " fresh") << '\n';
    return 0;
}

int TestCommand(int argc, char** argv)
{
    CommonCommandOptions options;

    for (int index = 2; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        auto parsedCommon = ParseCommonOption(argc, argv, index, options);
        if (parsedCommon.has_value()) {
            if (!parsedCommon->empty()) {
                std::cerr << *parsedCommon << "\n";
                PrintTestUsage();
                return 2;
            }
            continue;
        }
        if (argument == "--help") {
            PrintTestUsage();
            return 0;
        }

        PrintTestUsage();
        return 2;
    }

    std::filesystem::path projectRoot;
    scb::ProjectManifest manifest;
    bool hasManifest = false;
    std::string manifestSummary = "zero-config";

    const bool explicitManifestPath = options.manifestPath.has_value();
    const auto manifestLookup = FindManifestPath(std::move(options.manifestPath), projectRoot, hasManifest, manifest, manifestSummary);
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
    request.profile = options.profile;
    request.toolchain = detectedToolchain.toolchain;

    auto resolved = scb::ResolveProject(request);
    if (!resolved.ok()) {
        PrintDiagnostics(resolved.diagnostics);
        return 1;
    }

    AddDiscoveredTestTargets(resolved.project);
    const auto tests = std::count_if(resolved.project.targets.begin(), resolved.project.targets.end(), [](const scb::ResolvedTarget& target) {
        return target.id.kind == scb::TargetKind::TestExecutable;
    });
    if (tests == 0) {
        std::cout << "No tests found\n";
        return 0;
    }

    const auto plan = scb::PlanBuild({resolved.project});
    if (!plan.ok()) {
        PrintDiagnostics(plan.diagnostics);
        return 1;
    }

    const auto execution = scb::ExecuteBuild({plan.plan, options.dryRun, options.verbose, options.jobs});
    if (!execution.ok()) {
        std::map<std::string, const scb::ActionNode*> actionsById;
        for (const auto& planned : plan.plan.actions) {
            actionsById[planned.id] = &planned;
        }
        for (const auto& action : execution.summary.actions) {
            if (action.status != scb::ActionStatus::Failed) {
                continue;
            }
            const auto plannedIt = actionsById.find(action.actionId);
            if (plannedIt == actionsById.end()) {
                continue;
            }
            const auto& planned = *plannedIt->second;
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

    std::map<std::string, const scb::ActionNode*> actionsById;
    for (const auto& action : plan.plan.actions) {
        actionsById[action.id] = &action;
    }
    for (const auto& action : execution.summary.actions) {
        const auto plannedIt = actionsById.find(action.actionId);
        if (plannedIt == actionsById.end()) {
            continue;
        }
        const auto& planned = *plannedIt->second;
        if (options.dryRun) {
            std::cout << (action.status == scb::ActionStatus::Executed ? "Would execute " : "Would skip ")
                      << planned.label << '\n';
        } else if (action.status == scb::ActionStatus::Executed) {
            std::cout << planned.label << '\n';
        } else if (options.verbose) {
            std::cout << "Skip " << planned.label << '\n';
        }
    }

    const auto executables = TestExecutables(plan.plan);
    PrepareTestRuntimeEnvironment(plan.plan);
    std::size_t passed = 0;
    for (const auto& executable : executables) {
        if (options.dryRun) {
            std::cout << "Would run " << executable.relative << '\n';
            continue;
        }
        std::cout << "Run " << executable.relative << '\n';
        const int result = NormalizeSystemResult(std::system(QuoteShellPath(executable.absolute).c_str()));
        if (result != 0) {
            std::cerr << "test failed: " << executable.relative << " exited with " << result << '\n';
            return 1;
        }
        passed += 1;
    }

    if (options.dryRun) {
        std::cout << "Finished " << resolved.project.profile.name << ": " << executables.size() << " tests would run\n";
    } else {
        std::cout << "Finished " << resolved.project.profile.name << ": " << passed << " tests passed\n";
    }
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

    if (command == "test") {
        return TestCommand(argc, argv);
    }

    if (command == "clean") {
        return CleanCommand(argc, argv);
    }

    std::cerr << "unknown command: " << command << "\n\n";
    PrintUsage();
    return 2;
}
