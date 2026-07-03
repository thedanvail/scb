#include "scb/core/BuildPlan.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <sstream>
#include <set>
#include <string_view>

namespace scb {
namespace {

[[nodiscard]] bool HasErrors(const std::vector<Diagnostic>& diagnostics)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::Error;
    });
}

[[nodiscard]] std::string TargetKey(const TargetId& id)
{
    return ToString(id.kind) + ":" + id.name;
}

[[nodiscard]] std::string PathKey(const std::filesystem::path& root, const std::filesystem::path& path)
{
    const auto normalizedRoot = root.lexically_normal();
    const auto normalizedPath = path.lexically_normal();
    if (normalizedPath.is_relative()) {
        return normalizedPath.generic_string();
    }
    const auto relative = normalizedPath.lexically_relative(normalizedRoot);
    if (!relative.empty()) {
        bool escapesRoot = false;
        for (const auto& part : relative) {
            if (part == "..") {
                escapesRoot = true;
                break;
            }
        }
        if (!escapesRoot) {
            return relative.generic_string();
        }
    }
    return normalizedPath.generic_string();
}

[[nodiscard]] std::string ObjectExtension(const ToolchainInfo& toolchain)
{
    return toolchain.family == ToolchainFamily::Msvc ? ".obj" : ".o";
}

[[nodiscard]] std::string ExecutableExtension(const ToolchainInfo& toolchain)
{
    return toolchain.family == ToolchainFamily::Msvc ? ".exe" : "";
}

[[nodiscard]] std::string StaticLibraryName(const ToolchainInfo& toolchain, const std::string& name)
{
    if (toolchain.family == ToolchainFamily::Msvc) {
        return name + ".lib";
    }
    return "lib" + name + ".a";
}

[[nodiscard]] ProjectPath MakePath(const std::filesystem::path& root, const std::filesystem::path& relative)
{
    const auto normalizedRelative = relative.lexically_normal();
    return {(root / normalizedRelative).lexically_normal(), normalizedRelative.generic_string()};
}

[[nodiscard]] ProjectPath ObjectOutput(const ResolvedProject& project, const ResolvedTarget& target, const SourceFile& source)
{
    auto relative = std::filesystem::path(target.artifactDirectory.relative) / "obj" / std::filesystem::path(source.path.relative);
    relative += ObjectExtension(project.toolchain);
    return MakePath(project.root.absolute, relative);
}

[[nodiscard]] ProjectPath DepfileOutput(const ResolvedProject& project, const ProjectPath& objectFile)
{
    auto relative = std::filesystem::path(objectFile.relative);
    relative += ".d";
    return MakePath(project.root.absolute, relative);
}

[[nodiscard]] ProjectPath ArchiveOutput(const ResolvedProject& project, const ResolvedTarget& target)
{
    return MakePath(project.root.absolute, std::filesystem::path(target.artifactDirectory.relative) / StaticLibraryName(project.toolchain, target.id.name));
}

[[nodiscard]] ProjectPath ExecutableOutput(const ResolvedProject& project, const ResolvedTarget& target)
{
    return MakePath(project.root.absolute, std::filesystem::path(target.artifactDirectory.relative) / (target.id.name + ExecutableExtension(project.toolchain)));
}

[[nodiscard]] std::string ActionStateStem(const std::string& actionId)
{
    std::string sanitized;
    sanitized.reserve(actionId.size());
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char character : actionId) {
        hash ^= character;
        hash *= 1099511628211ull;
        sanitized.push_back(std::isalnum(character) ? static_cast<char>(character) : '_');
    }

    std::ostringstream stream;
    stream << sanitized << '_' << std::hex << hash;
    return stream.str();
}

[[nodiscard]] ProjectPath ActionStateOutput(const ResolvedProject& project, const ActionNode& action)
{
    const auto relative = std::filesystem::path("target") / project.profile.name / ".scb" / "actions" / (ActionStateStem(action.id) + ".toml");
    return MakePath(project.root.absolute, relative);
}

void AddOptimizationFlags(const ResolvedBuildOptions& build, ToolchainFamily family, std::vector<std::string>& args)
{
    if (family == ToolchainFamily::Msvc) {
        switch (build.optimization) {
        case OptimizationLevel::None:
        case OptimizationLevel::Debug:
            args.push_back("/Od");
            break;
        case OptimizationLevel::Speed:
            args.push_back("/O2");
            break;
        case OptimizationLevel::Size:
            args.push_back("/O1");
            break;
        }
        if (build.debugInfo) {
            args.push_back("/Zi");
        }
        return;
    }

    switch (build.optimization) {
    case OptimizationLevel::None:
    case OptimizationLevel::Debug:
        args.push_back("-O0");
        break;
    case OptimizationLevel::Speed:
        args.push_back("-O2");
        break;
    case OptimizationLevel::Size:
        args.push_back("-Os");
        break;
    }
    if (build.debugInfo) {
        args.push_back("-g");
    }
}

void AddStandardFlag(const ResolvedBuildOptions& build, ToolchainFamily family, std::vector<std::string>& args)
{
    if (family == ToolchainFamily::Msvc) {
        switch (build.standard) {
        case CxxStandard::Cxx17:
            args.push_back("/std:c++17");
            break;
        case CxxStandard::Cxx20:
            args.push_back("/std:c++20");
            break;
        case CxxStandard::Cxx23:
            args.push_back("/std:c++latest");
            break;
        }
        return;
    }

    args.push_back("-std=" + ToString(build.standard));
}

[[nodiscard]] std::vector<ResolvedIncludeDir> TransitiveIncludeDirs(
    const std::map<std::string, const ResolvedTarget*>& targetsByKey,
    const ResolvedTarget& target)
{
    std::set<std::string> visited;
    std::vector<ResolvedIncludeDir> includeDirs;
    std::set<std::string> seen;

    auto visit = [&](auto& self, const ResolvedTarget& current) -> void {
        const auto key = TargetKey(current.id);
        if (!visited.insert(key).second) {
            return;
        }
        for (const auto& includeDir : current.build.includeDirs) {
            if (seen.insert(includeDir.path.relative).second) {
                includeDirs.push_back(includeDir);
            }
        }
        for (const auto& dependency : current.dependencies) {
            const auto it = targetsByKey.find(TargetKey(dependency));
            if (it != targetsByKey.end()) {
                self(self, *it->second);
            }
        }
    };

    visit(visit, target);
    return includeDirs;
}

[[nodiscard]] std::vector<const ResolvedTarget*> OrderedLibraryDependencies(
    const std::map<std::string, const ResolvedTarget*>& targetsByKey,
    const ResolvedTarget& target)
{
    std::vector<const ResolvedTarget*> ordered;
    std::set<std::string> visited;

    auto visit = [&](auto& self, const ResolvedTarget& current) -> void {
        for (const auto& dependency : current.dependencies) {
            const auto it = targetsByKey.find(TargetKey(dependency));
            if (it == targetsByKey.end()) {
                continue;
            }
            const auto* targetDependency = it->second;
            const auto key = TargetKey(targetDependency->id);
            if (!visited.insert(key).second) {
                continue;
            }
            self(self, *targetDependency);
            if (targetDependency->id.kind == TargetKind::StaticLibrary) {
                ordered.push_back(targetDependency);
            }
        }
    };

    visit(visit, target);
    return ordered;
}

[[nodiscard]] CommandLine CompileCommand(
    const ResolvedProject& project,
    const ResolvedTarget& target,
    const SourceFile& source,
    const ProjectPath& objectFile,
    const std::vector<ResolvedIncludeDir>& includeDirs)
{
    CommandLine command;
    command.program = project.toolchain.compilerPath;
    command.workingDirectory = project.root.absolute;

    auto& args = command.args;
    if (project.toolchain.family == ToolchainFamily::Msvc) {
        args.push_back("/nologo");
        args.push_back("/c");
        AddStandardFlag(target.build, project.toolchain.family, args);
        AddOptimizationFlags(target.build, project.toolchain.family, args);
        for (const auto& [key, value] : target.build.defines) {
            args.push_back("/D" + key + "=" + value);
        }
        for (const auto& includeDir : includeDirs) {
            args.push_back("/I" + includeDir.path.absolute.string());
        }
        args.push_back("/Fo" + objectFile.absolute.string());
        args.insert(args.end(), target.build.compileFlags.begin(), target.build.compileFlags.end());
        args.push_back(source.path.absolute.string());
        return command;
    }

    args.push_back("-c");
    AddStandardFlag(target.build, project.toolchain.family, args);
    AddOptimizationFlags(target.build, project.toolchain.family, args);
    for (const auto& [key, value] : target.build.defines) {
        args.push_back("-D" + key + "=" + value);
    }
    for (const auto& includeDir : includeDirs) {
        args.push_back("-I" + includeDir.path.absolute.string());
    }
    args.insert(args.end(), target.build.compileFlags.begin(), target.build.compileFlags.end());
    const auto depfile = DepfileOutput(project, objectFile);
    args.push_back("-MMD");
    args.push_back("-MF");
    args.push_back(depfile.absolute.string());
    args.push_back("-MT");
    args.push_back(objectFile.absolute.string());
    args.push_back(source.path.absolute.string());
    args.push_back("-o");
    args.push_back(objectFile.absolute.string());
    return command;
}

[[nodiscard]] CommandLine ArchiveCommand(
    const ResolvedProject& project,
    const std::vector<ProjectPath>& objects,
    const ProjectPath& output)
{
    CommandLine command;
    command.program = project.toolchain.archiverPath;
    command.workingDirectory = project.root.absolute;

    auto& args = command.args;
    if (project.toolchain.family == ToolchainFamily::Msvc) {
        args.push_back("/nologo");
        args.push_back("/OUT:" + output.absolute.string());
        for (const auto& object : objects) {
            args.push_back(object.absolute.string());
        }
        return command;
    }

    args.push_back("rcs");
    args.push_back(output.absolute.string());
    for (const auto& object : objects) {
        args.push_back(object.absolute.string());
    }
    return command;
}

[[nodiscard]] CommandLine LinkCommand(
    const ResolvedProject& project,
    const ResolvedTarget& target,
    const std::vector<ProjectPath>& objects,
    const std::vector<ProjectPath>& libraries,
    const ProjectPath& output)
{
    CommandLine command;
    command.program = project.toolchain.compilerPath;
    command.workingDirectory = project.root.absolute;

    auto& args = command.args;
    if (project.toolchain.family == ToolchainFamily::Msvc) {
        args.push_back("/nologo");
        AddOptimizationFlags(target.build, project.toolchain.family, args);
        for (const auto& object : objects) {
            args.push_back(object.absolute.string());
        }
        for (const auto& library : libraries) {
            args.push_back(library.absolute.string());
        }
        args.push_back("/Fe:" + output.absolute.string());
        args.insert(args.end(), target.build.linkFlags.begin(), target.build.linkFlags.end());
        return command;
    }

    AddStandardFlag(target.build, project.toolchain.family, args);
    AddOptimizationFlags(target.build, project.toolchain.family, args);
    for (const auto& object : objects) {
        args.push_back(object.absolute.string());
    }
    for (const auto& library : libraries) {
        args.push_back(library.absolute.string());
    }
    args.insert(args.end(), target.build.linkFlags.begin(), target.build.linkFlags.end());
    args.push_back("-o");
    args.push_back(output.absolute.string());
    return command;
}

[[nodiscard]] ActionSignature MakeActionSignature(const ResolvedProject& project, const ActionNode& action)
{
    ActionSignature signature;
    signature.actionId = action.id;
    signature.kind = action.kind;
    signature.ownerTarget = TargetKey(action.owner);
    signature.program = PathKey(project.root.absolute, action.command.program);
    signature.args = action.command.args;
    signature.workingDirectory = action.command.workingDirectory.has_value()
        ? PathKey(project.root.absolute, *action.command.workingDirectory)
        : PathKey(project.root.absolute, project.root.absolute);
    signature.toolchainFamily = ToString(project.toolchain.family);
    signature.toolchainVersion = project.toolchain.version;
    signature.depfileFormat = action.depfileFormat;
    if (action.depfile.has_value()) {
        signature.depfilePath = action.depfile->relative;
    }
    for (const auto& input : action.inputs) {
        signature.explicitInputs.push_back(input.path.relative);
    }
    for (const auto& output : action.outputs) {
        signature.declaredOutputs.push_back(output.path.relative);
    }
    return signature;
}

void FinalizeAction(const ResolvedProject& project, ActionNode& action)
{
    action.stateFile = ActionStateOutput(project, action);
    action.signature = MakeActionSignature(project, action);
}

} // namespace

bool PlanBuildResult::ok() const
{
    return !HasErrors(diagnostics);
}

std::string ToString(ActionKind kind)
{
    switch (kind) {
    case ActionKind::Compile:
        return "compile";
    case ActionKind::Archive:
        return "archive";
    case ActionKind::Link:
        return "link";
    }
    return "unknown";
}

std::string ToString(DepfileFormat format)
{
    switch (format) {
    case DepfileFormat::None:
        return "none";
    case DepfileFormat::GnuMake:
        return "gnu-make";
    }
    return "unknown";
}

PlanBuildResult PlanBuild(const PlanBuildRequest& request)
{
    PlanBuildResult result;
    result.plan.project = request.project;

    if (request.project.toolchain.compilerPath.empty() || request.project.toolchain.family == ToolchainFamily::Unknown) {
        result.diagnostics.push_back({DiagnosticSeverity::Error, "toolchain detection is required before planning", std::nullopt});
        return result;
    }

    std::map<std::string, const ResolvedTarget*> targetsByKey;
    for (const auto& target : request.project.targets) {
        targetsByKey.emplace(TargetKey(target.id), &target);
    }

    std::map<std::string, std::vector<std::string>> compileActionsByTarget;
    std::map<std::string, std::vector<ProjectPath>> objectOutputsByTarget;
    std::map<std::string, std::string> finalActionByTarget;
    std::map<std::string, ProjectPath> finalOutputByTarget;

    for (const auto& target : request.project.targets) {
        const auto targetKey = TargetKey(target.id);

        if (target.id.kind == TargetKind::HeaderOnly) {
            continue;
        }
        if (target.id.kind == TargetKind::SharedLibrary) {
            result.diagnostics.push_back({DiagnosticSeverity::Error, "shared-library planning is not implemented yet for target " + targetKey, std::nullopt});
            continue;
        }

        const auto includeDirs = TransitiveIncludeDirs(targetsByKey, target);
        for (const auto& source : target.sources.compileSources) {
            const auto objectFile = ObjectOutput(request.project, target, source);
            ActionNode action;
            action.id = "compile:" + targetKey + ":" + source.path.relative;
            action.kind = ActionKind::Compile;
            action.owner = target.id;
            action.label = "Compile " + source.path.relative;
            action.inputs.push_back({source.path});
            action.outputs.push_back({objectFile});
            if (request.project.toolchain.family == ToolchainFamily::Gcc || request.project.toolchain.family == ToolchainFamily::Clang) {
                action.depfile = DepfileOutput(request.project, objectFile);
                action.depfileFormat = DepfileFormat::GnuMake;
            }
            action.command = CompileCommand(request.project, target, source, objectFile, includeDirs);
            FinalizeAction(request.project, action);

            compileActionsByTarget[targetKey].push_back(action.id);
            objectOutputsByTarget[targetKey].push_back(objectFile);
            result.plan.actions.push_back(std::move(action));
        }

        if (target.id.kind == TargetKind::StaticLibrary) {
            if (request.project.toolchain.archiverPath.empty()) {
                result.diagnostics.push_back({DiagnosticSeverity::Error, "no archiver found for static library target " + targetKey, std::nullopt});
                continue;
            }

            const auto output = ArchiveOutput(request.project, target);
            ActionNode action;
            action.id = "archive:" + targetKey;
            action.kind = ActionKind::Archive;
            action.owner = target.id;
            action.label = "Archive " + target.id.name;
            action.dependencies = compileActionsByTarget[targetKey];
            for (const auto& object : objectOutputsByTarget[targetKey]) {
                action.inputs.push_back({object});
            }
            action.outputs.push_back({output});
            action.command = ArchiveCommand(request.project, objectOutputsByTarget[targetKey], output);
            FinalizeAction(request.project, action);

            finalActionByTarget[targetKey] = action.id;
            finalOutputByTarget[targetKey] = output;
            result.plan.actions.push_back(std::move(action));
        }
    }

    for (const auto& target : request.project.targets) {
        if (target.id.kind != TargetKind::Executable) {
            continue;
        }

        const auto targetKey = TargetKey(target.id);
        const auto output = ExecutableOutput(request.project, target);
        const auto libraries = OrderedLibraryDependencies(targetsByKey, target);

        ActionNode action;
        action.id = "link:" + targetKey;
        action.kind = ActionKind::Link;
        action.owner = target.id;
        action.label = "Link " + target.id.name;
        action.dependencies = compileActionsByTarget[targetKey];

        std::vector<ProjectPath> libraryOutputs;
        for (const auto* library : libraries) {
            const auto libraryKey = TargetKey(library->id);
            if (finalActionByTarget.contains(libraryKey)) {
                action.dependencies.push_back(finalActionByTarget[libraryKey]);
                libraryOutputs.push_back(finalOutputByTarget[libraryKey]);
            }
        }

        for (const auto& object : objectOutputsByTarget[targetKey]) {
            action.inputs.push_back({object});
        }
        for (const auto& library : libraryOutputs) {
            action.inputs.push_back({library});
        }
        action.outputs.push_back({output});
        action.command = LinkCommand(request.project, target, objectOutputsByTarget[targetKey], libraryOutputs, output);
        FinalizeAction(request.project, action);

        finalActionByTarget[targetKey] = action.id;
        finalOutputByTarget[targetKey] = output;
        result.plan.actions.push_back(std::move(action));
    }

    return result;
}

} // namespace scb
