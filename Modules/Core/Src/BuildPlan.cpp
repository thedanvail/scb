#include "scb/core/BuildPlan.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
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

[[nodiscard]] std::string SharedLibraryName(const std::string& name)
{
#ifdef _WIN32
    return name + ".dll";
#elif defined(__APPLE__)
    return "lib" + name + ".dylib";
#else
    return "lib" + name + ".so";
#endif
}

[[nodiscard]] std::string SharedLibraryImportName(const std::string& name)
{
#ifdef _WIN32
    return name + ".lib";
#else
    return SharedLibraryName(name);
#endif
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
    if (project.toolchain.family == ToolchainFamily::Msvc) {
        relative += ".d.json";
    } else {
        relative += ".d";
    }
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

[[nodiscard]] ProjectPath SharedLibraryOutput(const ResolvedProject& project, const ResolvedTarget& target)
{
    return MakePath(project.root.absolute, std::filesystem::path(target.artifactDirectory.relative) / SharedLibraryName(target.id.name));
}

[[nodiscard]] ProjectPath SharedLibraryImportOutput(const ResolvedProject& project, const ResolvedTarget& target)
{
    return MakePath(project.root.absolute, std::filesystem::path(target.artifactDirectory.relative) / SharedLibraryImportName(target.id.name));
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

[[nodiscard]] std::vector<TargetId> SortedDependencies(const ResolvedTarget& target)
{
    std::vector<TargetId> sorted(target.dependencies);
    std::sort(sorted.begin(), sorted.end(), [](const TargetId& lhs, const TargetId& rhs) {
        return TargetKey(lhs) < TargetKey(rhs);
    });
    return sorted;
}

[[nodiscard]] std::vector<const ResolvedTarget*> OrderedLibraryDependencies(
    const std::map<std::string, const ResolvedTarget*>& targetsByKey,
    const ResolvedTarget& target)
{
    // Static libraries must be listed dependent-before-dependency on the link
    // command line. We collect libraries in post-order (dependency first) and
    // then reverse the result so that callers iterate top-down.
    std::vector<const ResolvedTarget*> postorder;
    std::set<std::string> visited;

    auto visit = [&](auto& self, const ResolvedTarget& current) -> void {
        for (const auto& dependency : SortedDependencies(current)) {
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
            if (targetDependency->id.kind == TargetKind::StaticLibrary ||
                targetDependency->id.kind == TargetKind::SharedLibrary) {
                postorder.push_back(targetDependency);
            }
        }
    };

    visit(visit, target);
    std::reverse(postorder.begin(), postorder.end());
    return postorder;
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
        const auto depfile = DepfileOutput(project, objectFile);
        args.push_back("/sourceDependencies");
        args.push_back(depfile.absolute.string());
        args.insert(args.end(), target.build.compileFlags.begin(), target.build.compileFlags.end());
        args.push_back(source.path.absolute.string());
        return command;
    }

    args.push_back("-c");
    AddStandardFlag(target.build, project.toolchain.family, args);
    AddOptimizationFlags(target.build, project.toolchain.family, args);
    if (target.id.kind == TargetKind::SharedLibrary) {
        args.push_back("-fPIC");
    }
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

    // Do not emit -std=c++NN when invoking the compiler as a link driver.
    // The language standard is a compile-time concern only.
    AddOptimizationFlags(target.build, project.toolchain.family, args);
    for (const auto& object : objects) {
        args.push_back(object.absolute.string());
    }
    for (const auto& library : libraries) {
        args.push_back(library.absolute.string());
    }
    args.insert(args.end(), target.build.linkFlags.begin(), target.build.linkFlags.end());

    // Embed rpaths so locally-built shared libraries can be found at runtime
    // without requiring LD_LIBRARY_PATH.
    std::set<std::string> rpathDirectories;
    for (const auto& library : libraries) {
        rpathDirectories.insert(library.absolute.parent_path().string());
    }
    for (const auto& directory : rpathDirectories) {
        args.push_back("-Wl,-rpath," + directory);
    }

    args.push_back("-o");
    args.push_back(output.absolute.string());
    return command;
}

[[nodiscard]] CommandLine SharedLibraryLinkCommand(
    const ResolvedProject& project,
    const ResolvedTarget& target,
    const std::vector<ProjectPath>& objects,
    const std::vector<ProjectPath>& libraries,
    const ProjectPath& output,
    const ProjectPath& importOutput)
{
    CommandLine command;
    command.workingDirectory = project.root.absolute;

    auto& args = command.args;
    if (project.toolchain.family == ToolchainFamily::Msvc) {
        command.program = project.toolchain.linkerPath.empty()
            ? project.toolchain.compilerPath
            : project.toolchain.linkerPath;
        args.push_back("/nologo");
        args.push_back("/DLL");
        args.push_back("/OUT:" + output.absolute.string());
        args.push_back("/IMPLIB:" + importOutput.absolute.string());
        for (const auto& object : objects) {
            args.push_back(object.absolute.string());
        }
        for (const auto& library : libraries) {
            args.push_back(library.absolute.string());
        }
        args.insert(args.end(), target.build.linkFlags.begin(), target.build.linkFlags.end());
        return command;
    }

    command.program = project.toolchain.compilerPath;
    args.push_back("-shared");
    args.push_back("-fPIC");
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
    signature.toolchainIdentity = project.toolchain.identity;
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
    case DepfileFormat::SourceDependencies:
        return "source-dependencies";
    }
    return "unknown";
}

namespace {

void WriteJsonString(std::ostringstream& stream, std::string_view value)
{
    stream << '"';
    for (const char character : value) {
        switch (character) {
        case '"':
            stream << "\\\"";
            break;
        case '\\':
            stream << "\\\\";
            break;
        case '\b':
            stream << "\\b";
            break;
        case '\f':
            stream << "\\f";
            break;
        case '\n':
            stream << "\\n";
            break;
        case '\r':
            stream << "\\r";
            break;
        case '\t':
            stream << "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) {
                char buffer[7];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", character);
                stream << buffer;
            } else {
                stream << character;
            }
        }
    }
    stream << '"';
}

void WriteJsonPath(std::ostringstream& stream, const ProjectPath& path)
{
    stream << "{\n";
    stream << "        \"absolute\": ";
    WriteJsonString(stream, path.absolute.string());
    stream << ",\n";
    stream << "        \"relative\": ";
    WriteJsonString(stream, path.relative);
    stream << "\n      }";
}

} // namespace

std::string ToJson(const BuildPlan& plan)
{
    std::ostringstream stream;
    stream << "{\n";
    stream << "  \"project\": {\n";
    stream << "    \"name\": ";
    WriteJsonString(stream, plan.project.name);
    stream << ",\n";
    stream << "    \"root\": ";
    WriteJsonString(stream, plan.project.root.absolute.string());
    stream << ",\n";
    stream << "    \"profile\": ";
    WriteJsonString(stream, plan.project.profile.name);
    stream << ",\n";
    stream << "    \"toolchain\": {\n";
    stream << "      \"family\": ";
    WriteJsonString(stream, ToString(plan.project.toolchain.family));
    stream << ",\n";
    stream << "      \"compiler\": ";
    WriteJsonString(stream, plan.project.toolchain.compilerPath);
    stream << ",\n";
    stream << "      \"archiver\": ";
    WriteJsonString(stream, plan.project.toolchain.archiverPath);
    stream << ",\n";
    stream << "      \"linker\": ";
    WriteJsonString(stream, plan.project.toolchain.linkerPath);
    stream << ",\n";
    stream << "      \"version\": ";
    WriteJsonString(stream, plan.project.toolchain.version);
    stream << ",\n";
    stream << "      \"identity\": ";
    WriteJsonString(stream, plan.project.toolchain.identity);
    stream << "\n    }\n";
    stream << "  },\n";
    stream << "  \"actions\": [\n";

    for (std::size_t actionIndex = 0; actionIndex < plan.actions.size(); ++actionIndex) {
        const auto& action = plan.actions[actionIndex];
        stream << "    {\n";
        stream << "      \"id\": ";
        WriteJsonString(stream, action.id);
        stream << ",\n";
        stream << "      \"kind\": ";
        WriteJsonString(stream, ToString(action.kind));
        stream << ",\n";
        stream << "      \"owner\": ";
        WriteJsonString(stream, ToString(action.owner.kind) + ":" + action.owner.name);
        stream << ",\n";
        stream << "      \"label\": ";
        WriteJsonString(stream, action.label);
        stream << ",\n";

        stream << "      \"inputs\": [\n";
        for (std::size_t index = 0; index < action.inputs.size(); ++index) {
            stream << "        ";
            WriteJsonPath(stream, action.inputs[index].path);
            if (index + 1 < action.inputs.size()) {
                stream << ",";
            }
            stream << "\n";
        }
        stream << "      ],\n";

        stream << "      \"outputs\": [\n";
        for (std::size_t index = 0; index < action.outputs.size(); ++index) {
            stream << "        ";
            WriteJsonPath(stream, action.outputs[index].path);
            if (index + 1 < action.outputs.size()) {
                stream << ",";
            }
            stream << "\n";
        }
        stream << "      ],\n";

        stream << "      \"dependencies\": [\n";
        for (std::size_t index = 0; index < action.dependencies.size(); ++index) {
            stream << "        ";
            WriteJsonString(stream, action.dependencies[index]);
            if (index + 1 < action.dependencies.size()) {
                stream << ",";
            }
            stream << "\n";
        }
        stream << "      ],\n";

        stream << "      \"depfile\": ";
        if (action.depfile.has_value()) {
            WriteJsonPath(stream, *action.depfile);
        } else {
            stream << "null";
        }
        stream << ",\n";
        stream << "      \"depfile_format\": ";
        WriteJsonString(stream, ToString(action.depfileFormat));
        stream << ",\n";

        stream << "      \"command\": {\n";
        stream << "        \"program\": ";
        WriteJsonString(stream, action.command.program);
        stream << ",\n";
        stream << "        \"args\": [\n";
        for (std::size_t index = 0; index < action.command.args.size(); ++index) {
            stream << "          ";
            WriteJsonString(stream, action.command.args[index]);
            if (index + 1 < action.command.args.size()) {
                stream << ",";
            }
            stream << "\n";
        }
        stream << "        ],\n";
        stream << "        \"working_directory\": ";
        if (action.command.workingDirectory.has_value()) {
            WriteJsonString(stream, action.command.workingDirectory->string());
        } else {
            stream << "null";
        }
        stream << "\n      }\n";

        stream << "    }";
        if (actionIndex + 1 < plan.actions.size()) {
            stream << ",";
        }
        stream << "\n";
    }

    stream << "  ]\n";
    stream << "}\n";
    return stream.str();
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
            if (request.project.toolchain.family == ToolchainFamily::Gcc ||
                request.project.toolchain.family == ToolchainFamily::Clang) {
                action.depfile = DepfileOutput(request.project, objectFile);
                action.depfileFormat = DepfileFormat::GnuMake;
            } else if (request.project.toolchain.family == ToolchainFamily::Msvc) {
                action.depfile = DepfileOutput(request.project, objectFile);
                action.depfileFormat = DepfileFormat::SourceDependencies;
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
        } else if (target.id.kind == TargetKind::SharedLibrary) {
            const auto output = SharedLibraryOutput(request.project, target);
            const auto importOutput = SharedLibraryImportOutput(request.project, target);
            const auto orderedLibraries = OrderedLibraryDependencies(targetsByKey, target);
            std::vector<ProjectPath> libraryInputs;
            std::vector<std::string> libraryDependencies;
            for (const auto* library : orderedLibraries) {
                const auto libraryKey = TargetKey(library->id);
                if (finalActionByTarget.contains(libraryKey)) {
                    libraryDependencies.push_back(finalActionByTarget[libraryKey]);
                    if (library->id.kind == TargetKind::StaticLibrary) {
                        libraryInputs.push_back(finalOutputByTarget[libraryKey]);
                    } else {
                        libraryInputs.push_back(SharedLibraryImportOutput(request.project, *library));
                    }
                }
            }

            ActionNode action;
            action.id = "link:" + targetKey;
            action.kind = ActionKind::Link;
            action.owner = target.id;
            action.label = "Link shared library " + target.id.name;
            action.dependencies = compileActionsByTarget[targetKey];
            action.dependencies.insert(action.dependencies.end(), libraryDependencies.begin(), libraryDependencies.end());
            for (const auto& object : objectOutputsByTarget[targetKey]) {
                action.inputs.push_back({object});
            }
            for (const auto& library : libraryInputs) {
                action.inputs.push_back({library});
            }
            action.outputs.push_back({output});
            if (output.absolute != importOutput.absolute) {
                action.outputs.push_back({importOutput});
            }
            action.command = SharedLibraryLinkCommand(
                request.project, target, objectOutputsByTarget[targetKey], libraryInputs, output, importOutput);
            FinalizeAction(request.project, action);

            finalActionByTarget[targetKey] = action.id;
            finalOutputByTarget[targetKey] = importOutput;
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
