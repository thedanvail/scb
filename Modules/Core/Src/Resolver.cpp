#include "scb/core/ResolvedProject.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string_view>

namespace scb {
namespace {

struct PathPattern {
    std::string pattern;
    bool recursive = false;
    std::filesystem::path directory;
    std::string suffix;
};

struct ResolvedPath {
    bool valid = false;
    ProjectPath path;
};

struct PendingBuildOptions {
    std::vector<std::string> includeDirs;
    std::map<std::string, std::string> defines;
    std::vector<std::string> compileFlags;
    std::vector<std::string> linkFlags;
    CxxStandard standard = CxxStandard::Cxx20;
    OptimizationLevel optimization = OptimizationLevel::Debug;
    bool debugInfo = true;
};

struct TargetDeclaration {
    ManifestTarget manifestTarget;
    TargetOrigin origin = TargetOrigin::Explicit;
};

[[nodiscard]] bool IsCompileSource(const std::filesystem::path& path)
{
    const auto extension = path.extension().string();
    return extension == ".cpp" || extension == ".cc" || extension == ".cxx" || extension == ".c++";
}

[[nodiscard]] bool IsHeader(const std::filesystem::path& path)
{
    const auto extension = path.extension().string();
    return extension == ".h" || extension == ".hh" || extension == ".hpp" || extension == ".hxx";
}

[[nodiscard]] bool IsValidTargetName(const std::string& name)
{
    if (name.empty() || !std::isalpha(static_cast<unsigned char>(name.front()))) {
        return false;
    }

    return std::all_of(name.begin(), name.end(), [](char character) {
        const auto value = static_cast<unsigned char>(character);
        return std::isalnum(value) || character == '_' || character == '.' || character == '-';
    });
}

[[nodiscard]] std::string SlashPath(const std::filesystem::path& path)
{
    return path.generic_string();
}

[[nodiscard]] std::filesystem::path LexicalRoot(const std::filesystem::path& root)
{
    auto normalized = root.lexically_normal();
    if (normalized.is_relative()) {
        normalized = std::filesystem::absolute(normalized).lexically_normal();
    }
    return normalized;
}

[[nodiscard]] std::string RelativeToRoot(const std::filesystem::path& root, const std::filesystem::path& absolute)
{
    return SlashPath(absolute.lexically_relative(root));
}

[[nodiscard]] bool EscapesRoot(const std::filesystem::path& relative)
{
    for (const auto& part : relative) {
        if (part == "..") {
            return true;
        }
    }
    return false;
}

[[nodiscard]] ResolvedPath ResolveRootPath(
    const std::filesystem::path& root,
    const std::string& raw,
    std::vector<Diagnostic>& diagnostics,
    std::string_view context)
{
    if (raw.empty()) {
        diagnostics.push_back({DiagnosticSeverity::Error, std::string(context) + " path must not be empty"});
        return {};
    }

    const std::filesystem::path path(raw);
    if (path.is_absolute()) {
        diagnostics.push_back({DiagnosticSeverity::Error, std::string(context) + " path must be project-relative: " + raw});
        return {};
    }

    const auto relative = path.lexically_normal();
    if (EscapesRoot(relative)) {
        diagnostics.push_back({DiagnosticSeverity::Error, std::string(context) + " path escapes the project root: " + raw});
        return {};
    }

    const auto absolute = (root / relative).lexically_normal();
    return {true, {absolute, SlashPath(relative)}};
}

void Deduplicate(std::vector<std::string>& values)
{
    std::set<std::string> seen;
    std::vector<std::string> output;
    output.reserve(values.size());
    for (const auto& value : values) {
        if (seen.insert(value).second) {
            output.push_back(value);
        }
    }
    values = std::move(output);
}

[[nodiscard]] std::optional<CxxStandard> ParseStandard(const std::string& value)
{
    if (value.empty() || value == "c++20") {
        return CxxStandard::Cxx20;
    }
    if (value == "c++17") {
        return CxxStandard::Cxx17;
    }
    if (value == "c++23") {
        return CxxStandard::Cxx23;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<OptimizationLevel> ParseOptimization(const std::string& value)
{
    if (value.empty() || value == "debug") {
        return OptimizationLevel::Debug;
    }
    if (value == "none") {
        return OptimizationLevel::None;
    }
    if (value == "speed") {
        return OptimizationLevel::Speed;
    }
    if (value == "size") {
        return OptimizationLevel::Size;
    }
    return std::nullopt;
}

void ApplyBuildOptions(
    PendingBuildOptions& resolved,
    const ManifestBuildOptions& manifest,
    std::vector<Diagnostic>& diagnostics,
    std::string_view context)
{
    resolved.includeDirs.insert(resolved.includeDirs.end(), manifest.includeDirs.begin(), manifest.includeDirs.end());
    resolved.compileFlags.insert(resolved.compileFlags.end(), manifest.compileFlags.begin(), manifest.compileFlags.end());
    resolved.linkFlags.insert(resolved.linkFlags.end(), manifest.linkFlags.begin(), manifest.linkFlags.end());

    for (const auto& [key, value] : manifest.defines) {
        resolved.defines[key] = value;
    }

    if (!manifest.standard.empty()) {
        const auto standard = ParseStandard(manifest.standard);
        if (!standard.has_value()) {
            diagnostics.push_back({DiagnosticSeverity::Error, std::string(context) + " uses unsupported C++ standard: " + manifest.standard});
        } else {
            resolved.standard = *standard;
        }
    }

    if (!manifest.optimization.empty()) {
        const auto optimization = ParseOptimization(manifest.optimization);
        if (!optimization.has_value()) {
            diagnostics.push_back({DiagnosticSeverity::Error, std::string(context) + " uses unsupported optimization level: " + manifest.optimization});
        } else {
            resolved.optimization = *optimization;
        }
    }

    if (manifest.hasDebugInfo) {
        resolved.debugInfo = manifest.debugInfo;
    }
}

void ApplyTargetOptions(PendingBuildOptions& resolved, const ManifestTarget& target, std::vector<Diagnostic>& diagnostics)
{
    ManifestBuildOptions options;
    options.includeDirs = target.includeDirs;
    options.defines = target.defines;
    options.compileFlags = target.compileFlags;
    options.linkFlags = target.linkFlags;
    options.standard = target.standard;
    ApplyBuildOptions(resolved, options, diagnostics, "target '" + target.name + "'");
}

[[nodiscard]] PendingBuildOptions DefaultBuildOptions(const std::string& profile)
{
    PendingBuildOptions options;
    options.standard = CxxStandard::Cxx20;
    options.optimization = profile == "release" ? OptimizationLevel::Speed : OptimizationLevel::Debug;
    options.debugInfo = profile != "release";
    return options;
}

[[nodiscard]] ResolvedBuildOptions NormalizeBuildOptions(
    const std::filesystem::path& root,
    const PendingBuildOptions& pending,
    std::vector<Diagnostic>& diagnostics,
    std::string_view context)
{
    ResolvedBuildOptions build;
    build.defines = pending.defines;
    build.compileFlags = pending.compileFlags;
    build.linkFlags = pending.linkFlags;
    build.standard = pending.standard;
    build.optimization = pending.optimization;
    build.debugInfo = pending.debugInfo;

    std::set<std::string> includeSeen;
    for (const auto& includeDir : pending.includeDirs) {
        auto path = ResolveRootPath(root, includeDir, diagnostics, context);
        if (!path.valid) {
            continue;
        }
        if (!std::filesystem::exists(path.path.absolute) || !std::filesystem::is_directory(path.path.absolute)) {
            diagnostics.push_back({DiagnosticSeverity::Error, std::string(context) + " include directory does not exist: " + includeDir});
            continue;
        }
        if (includeSeen.insert(path.path.relative).second) {
            build.includeDirs.push_back({path.path});
        }
    }
    Deduplicate(build.compileFlags);
    Deduplicate(build.linkFlags);
    return build;
}

[[nodiscard]] std::optional<PathPattern> ParsePattern(const std::string& pattern)
{
    const auto marker = pattern.find("**/");
    if (marker != std::string::npos) {
        const auto directory = pattern.substr(0, marker);
        auto suffix = pattern.substr(marker + 3);
        if (suffix.starts_with('*')) {
            suffix.erase(suffix.begin());
        }
        if (suffix.empty() || suffix.find('*') != std::string::npos) {
            return std::nullopt;
        }
        return PathPattern{pattern, true, directory.empty() ? "." : directory, suffix};
    }

    const auto star = pattern.find('*');
    if (star != std::string::npos) {
        const auto slash = pattern.rfind('/', star);
        const auto directory = slash == std::string::npos ? "." : pattern.substr(0, slash);
        auto suffix = pattern.substr(star + 1);
        if (suffix.empty() || suffix.find('*') != std::string::npos) {
            return std::nullopt;
        }
        return PathPattern{pattern, false, directory, suffix};
    }

    return PathPattern{pattern, false, pattern, ""};
}

[[nodiscard]] bool EndsWith(const std::string& value, const std::string& suffix)
{
    return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

[[nodiscard]] std::vector<ProjectPath> ExpandPattern(
    const std::filesystem::path& root,
    const std::string& pattern,
    std::vector<Diagnostic>& diagnostics,
    std::string_view context,
    bool requireMatch)
{
    const auto parsed = ParsePattern(pattern);
    if (!parsed.has_value()) {
        diagnostics.push_back({DiagnosticSeverity::Error, std::string(context) + " uses unsupported glob pattern: " + pattern});
        return {};
    }

    if (pattern.find('*') == std::string::npos) {
        auto path = ResolveRootPath(root, pattern, diagnostics, context);
        if (!path.valid) {
            return {};
        }
        if (!std::filesystem::exists(path.path.absolute) || !std::filesystem::is_regular_file(path.path.absolute)) {
            if (requireMatch) {
                diagnostics.push_back({DiagnosticSeverity::Error, std::string(context) + " source does not exist: " + pattern});
            }
            return {};
        }
        return {path.path};
    }

    auto directory = ResolveRootPath(root, SlashPath(parsed->directory), diagnostics, context);
    if (!directory.valid) {
        return {};
    }

    std::vector<ProjectPath> matches;
    if (std::filesystem::exists(directory.path.absolute) && std::filesystem::is_directory(directory.path.absolute)) {
        if (parsed->recursive) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(directory.path.absolute)) {
                if (entry.is_regular_file() && EndsWith(SlashPath(entry.path().lexically_relative(directory.path.absolute)), parsed->suffix)) {
                    const auto absolute = entry.path().lexically_normal();
                    matches.push_back({absolute, RelativeToRoot(root, absolute)});
                }
            }
        } else {
            for (const auto& entry : std::filesystem::directory_iterator(directory.path.absolute)) {
                if (entry.is_regular_file() && EndsWith(entry.path().filename().string(), parsed->suffix)) {
                    const auto absolute = entry.path().lexically_normal();
                    matches.push_back({absolute, RelativeToRoot(root, absolute)});
                }
            }
        }
    }

    std::sort(matches.begin(), matches.end(), [](const ProjectPath& lhs, const ProjectPath& rhs) {
        return lhs.relative < rhs.relative;
    });

    if (requireMatch && matches.empty()) {
        diagnostics.push_back({DiagnosticSeverity::Error, std::string(context) + " pattern matched no files: " + pattern});
    }

    return matches;
}

[[nodiscard]] SourceSet ResolveSourceSet(
    const std::filesystem::path& root,
    const ManifestSourceSet& sourceSet,
    std::vector<Diagnostic>& diagnostics,
    std::string_view context)
{
    std::set<std::string> excluded;
    for (const auto& pattern : sourceSet.exclude) {
        for (const auto& path : ExpandPattern(root, pattern, diagnostics, context, false)) {
            excluded.insert(path.relative);
        }
    }

    std::map<std::string, ProjectPath> included;
    for (const auto& pattern : sourceSet.include) {
        for (const auto& path : ExpandPattern(root, pattern, diagnostics, context, true)) {
            if (!excluded.contains(path.relative)) {
                included[path.relative] = path;
            }
        }
    }

    SourceSet result;
    for (const auto& [_, path] : included) {
        if (IsCompileSource(path.absolute)) {
            result.compileSources.push_back({path});
        } else if (IsHeader(path.absolute)) {
            result.headers.push_back({path});
        }
    }
    return result;
}

[[nodiscard]] ManifestTarget MakeTarget(
    std::string name,
    TargetKind kind,
    std::vector<std::string> includes,
    std::vector<std::string> excludes = {})
{
    ManifestTarget target;
    target.name = std::move(name);
    target.kind = kind;
    target.sources.include = std::move(includes);
    target.sources.exclude = std::move(excludes);
    return target;
}

[[nodiscard]] bool DirectoryContainsExtension(const std::filesystem::path& directory, const std::string& extension)
{
    if (!std::filesystem::exists(directory) || !std::filesystem::is_directory(directory)) {
        return false;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == extension) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::vector<ManifestTarget> InferTargets(const std::filesystem::path& root, const std::string& projectName)
{
    std::vector<ManifestTarget> inferred;
    const auto mainCpp = root / "src" / "main.cpp";
    const auto libCpp = root / "src" / "lib.cpp";
    const auto includeDir = root / "include";
    const bool hasMain = std::filesystem::exists(mainCpp);
    const bool hasLibCpp = std::filesystem::exists(libCpp);
    const bool hasIncludeDir = std::filesystem::exists(includeDir) && std::filesystem::is_directory(includeDir);

    const bool inferLibrary = hasLibCpp || hasIncludeDir;
    const bool inferExecutable = hasMain;

    if (inferLibrary) {
        if (hasLibCpp) {
            inferred.push_back(MakeTarget(projectName, TargetKind::StaticLibrary, {"src/**/*.cpp"}, {"src/main.cpp"}));
        } else {
            std::vector<std::string> headerPatterns;
            for (const auto& extension : {std::string(".h"), std::string(".hh"), std::string(".hpp"), std::string(".hxx")}) {
                if (DirectoryContainsExtension(includeDir, extension)) {
                    headerPatterns.push_back("include/**/*" + extension);
                }
            }

            auto target = MakeTarget(projectName, TargetKind::HeaderOnly, std::move(headerPatterns));
            target.includeDirs = {"include"};
            inferred.push_back(std::move(target));
        }
    }

    if (inferExecutable) {
        auto target = MakeTarget(projectName, TargetKind::Executable, inferLibrary ? std::vector<std::string>{"src/main.cpp"} : std::vector<std::string>{"src/**/*.cpp"});
        if (inferLibrary) {
            target.dependencies.push_back({std::string("lib:") + projectName});
        }
        inferred.push_back(std::move(target));
    }

    return inferred;
}

[[nodiscard]] std::vector<TargetDeclaration> CollectTargetDeclarations(
    const std::filesystem::path& root,
    const ProjectManifest& manifest,
    const std::string& projectName)
{
    std::vector<TargetDeclaration> declarations;
    if (!manifest.targets.empty()) {
        declarations.reserve(manifest.targets.size());
        for (const auto& target : manifest.targets) {
            declarations.push_back({target, TargetOrigin::Explicit});
        }
        return declarations;
    }

    auto inferred = InferTargets(root, projectName);
    declarations.reserve(inferred.size());
    for (auto& target : inferred) {
        declarations.push_back({std::move(target), TargetOrigin::Inferred});
    }
    return declarations;
}

[[nodiscard]] std::string TargetKey(const TargetId& id)
{
    return ToString(id.kind) + ":" + id.name;
}

[[nodiscard]] std::optional<TargetKind> ParseDependencyKind(const std::string& value)
{
    if (value == "exe") {
        return TargetKind::Executable;
    }
    if (value == "lib" || value == "static-lib") {
        return TargetKind::StaticLibrary;
    }
    if (value == "shared-lib") {
        return TargetKind::SharedLibrary;
    }
    if (value == "header-lib" || value == "header-only") {
        return TargetKind::HeaderOnly;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<TargetId> ResolveDependency(
    const ManifestDependency& dependency,
    const std::map<std::string, TargetId>& byKey,
    const std::vector<TargetId>& targets,
    std::vector<Diagnostic>& diagnostics,
    std::string_view context)
{
    const auto colon = dependency.value.find(':');
    if (colon != std::string::npos) {
        const auto kind = ParseDependencyKind(dependency.value.substr(0, colon));
        const auto name = dependency.value.substr(colon + 1);
        if (!kind.has_value() || name.empty()) {
            diagnostics.push_back({DiagnosticSeverity::Error, std::string(context) + " has invalid dependency: " + dependency.value});
            return std::nullopt;
        }

        TargetId id{*kind, name};
        if (!byKey.contains(TargetKey(id))) {
            diagnostics.push_back({DiagnosticSeverity::Error, std::string(context) + " references unknown dependency: " + dependency.value});
            return std::nullopt;
        }
        if (id.kind == TargetKind::Executable) {
            diagnostics.push_back({DiagnosticSeverity::Error, std::string(context) + " cannot depend on executable target: " + dependency.value});
            return std::nullopt;
        }
        return id;
    }

    std::vector<TargetId> matches;
    for (const auto& target : targets) {
        if (target.name == dependency.value && target.kind != TargetKind::Executable) {
            matches.push_back(target);
        }
    }

    if (matches.empty()) {
        diagnostics.push_back({DiagnosticSeverity::Error, std::string(context) + " references unknown dependency: " + dependency.value});
        return std::nullopt;
    }
    if (matches.size() > 1) {
        diagnostics.push_back({DiagnosticSeverity::Error, std::string(context) + " has ambiguous dependency: " + dependency.value});
        return std::nullopt;
    }
    return matches.front();
}

bool HasDependencyCycle(
    const std::vector<ResolvedTarget>& targets,
    std::vector<Diagnostic>& diagnostics)
{
    std::map<std::string, std::vector<std::string>> graph;
    for (const auto& target : targets) {
        auto& dependencies = graph[TargetKey(target.id)];
        for (const auto& dependency : target.dependencies) {
            dependencies.push_back(TargetKey(dependency));
        }
    }

    std::set<std::string> visiting;
    std::set<std::string> visited;

    auto visit = [&](auto& self, const std::string& key) -> bool {
        if (visited.contains(key)) {
            return false;
        }
        if (visiting.contains(key)) {
            diagnostics.push_back({DiagnosticSeverity::Error, "target dependency cycle includes " + key});
            return true;
        }
        visiting.insert(key);
        for (const auto& dependency : graph[key]) {
            if (self(self, dependency)) {
                return true;
            }
        }
        visiting.erase(key);
        visited.insert(key);
        return false;
    };

    bool cycle = false;
    for (const auto& [key, _] : graph) {
        cycle = visit(visit, key) || cycle;
    }
    return cycle;
}

[[nodiscard]] ProjectPath ArtifactDirectory(const std::filesystem::path& root, const std::string& profile, const TargetId& id)
{
    const auto relative = std::filesystem::path("target") / profile / ToString(id.kind) / id.name;
    return {(root / relative).lexically_normal(), SlashPath(relative)};
}

} // namespace

bool ResolveResult::ok() const
{
    return std::none_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::Error;
    });
}

std::string ToString(TargetKind kind)
{
    switch (kind) {
    case TargetKind::Executable:
        return "exe";
    case TargetKind::StaticLibrary:
        return "lib";
    case TargetKind::SharedLibrary:
        return "shared-lib";
    case TargetKind::HeaderOnly:
        return "header-lib";
    }
    return "unknown";
}

std::string ToString(CxxStandard standard)
{
    switch (standard) {
    case CxxStandard::Cxx17:
        return "c++17";
    case CxxStandard::Cxx20:
        return "c++20";
    case CxxStandard::Cxx23:
        return "c++23";
    }
    return "unknown";
}

std::string ToString(OptimizationLevel optimization)
{
    switch (optimization) {
    case OptimizationLevel::None:
        return "none";
    case OptimizationLevel::Debug:
        return "debug";
    case OptimizationLevel::Speed:
        return "speed";
    case OptimizationLevel::Size:
        return "size";
    }
    return "unknown";
}

std::string ToString(ToolchainFamily family)
{
    switch (family) {
    case ToolchainFamily::Unknown:
        return "unknown";
    case ToolchainFamily::Gcc:
        return "gcc";
    case ToolchainFamily::Clang:
        return "clang";
    case ToolchainFamily::Msvc:
        return "msvc";
    }
    return "unknown";
}

std::string ToString(TargetOrigin origin)
{
    switch (origin) {
    case TargetOrigin::Explicit:
        return "explicit";
    case TargetOrigin::Inferred:
        return "inferred";
    }
    return "unknown";
}

ResolveResult ResolveProject(const ResolveRequest& request)
{
    ResolveResult result;
    const auto root = LexicalRoot(request.projectRoot);

    const auto projectName = request.manifest.project.name.empty() ? root.filename().string() : request.manifest.project.name;
    auto projectBuild = request.manifest.build;
    if (!request.manifest.project.standard.empty() && projectBuild.standard.empty()) {
        projectBuild.standard = request.manifest.project.standard;
    }
    const auto targetDeclarations = CollectTargetDeclarations(root, request.manifest, projectName);

    result.project.name = projectName;
    result.project.root = {root, "."};
    result.project.profile.name = request.profile.empty() ? "debug" : request.profile;
    result.project.toolchain = request.toolchain;

    auto profileBuild = DefaultBuildOptions(result.project.profile.name);
    ApplyBuildOptions(profileBuild, projectBuild, result.diagnostics, "project build options");

    auto profile = request.manifest.profiles.find(result.project.profile.name);
    if (profile != request.manifest.profiles.end()) {
        ApplyBuildOptions(profileBuild, profile->second.build, result.diagnostics, "profile '" + result.project.profile.name + "'");
    } else if (result.project.profile.name != "debug" && result.project.profile.name != "release") {
        result.diagnostics.push_back({DiagnosticSeverity::Error, "unknown profile: " + result.project.profile.name});
    }
    result.project.profile.build = NormalizeBuildOptions(root, profileBuild, result.diagnostics, "profile '" + result.project.profile.name + "'");

    std::map<std::string, TargetId> byKey;
    std::vector<TargetId> ids;
    for (const auto& declaration : targetDeclarations) {
        const auto& target = declaration.manifestTarget;
        const TargetId id{target.kind, target.name};
        if (!IsValidTargetName(target.name)) {
            result.diagnostics.push_back({DiagnosticSeverity::Error, "invalid target name: " + target.name});
        }
        const auto key = TargetKey(id);
        if (byKey.contains(key)) {
            result.diagnostics.push_back({DiagnosticSeverity::Error, "duplicate target: " + key});
        }
        byKey[key] = id;
        ids.push_back(id);
    }

    for (const auto& declaration : targetDeclarations) {
        const auto& target = declaration.manifestTarget;
        ResolvedTarget resolved;
        resolved.id = {target.kind, target.name};
        resolved.origin = declaration.origin;
        resolved.artifactDirectory = ArtifactDirectory(root, result.project.profile.name, resolved.id);
        auto targetBuild = profileBuild;
        ApplyTargetOptions(targetBuild, target, result.diagnostics);
        resolved.build = NormalizeBuildOptions(root, targetBuild, result.diagnostics, "target '" + target.name + "'");
        resolved.sources = ResolveSourceSet(root, target.sources, result.diagnostics, "target '" + target.name + "'");

        for (const auto& dependency : target.dependencies) {
            if (auto resolvedDependency = ResolveDependency(dependency, byKey, ids, result.diagnostics, "target '" + target.name + "'")) {
                resolved.dependencies.push_back(*resolvedDependency);
            }
        }

        if (target.kind == TargetKind::HeaderOnly) {
            if (!resolved.sources.compileSources.empty()) {
                result.diagnostics.push_back({DiagnosticSeverity::Error, "header-only target must not have compile sources: " + target.name});
            }
            if (resolved.sources.headers.empty() && resolved.build.includeDirs.empty()) {
                result.diagnostics.push_back({DiagnosticSeverity::Error, "header-only target has no headers or include directories: " + target.name});
            }
        } else if (resolved.sources.compileSources.empty()) {
            result.diagnostics.push_back({DiagnosticSeverity::Error, "target has no compile sources: " + TargetKey(resolved.id)});
        }

        result.project.targets.push_back(std::move(resolved));
    }

    std::sort(result.project.targets.begin(), result.project.targets.end(), [](const ResolvedTarget& lhs, const ResolvedTarget& rhs) {
        return TargetKey(lhs.id) < TargetKey(rhs.id);
    });

    HasDependencyCycle(result.project.targets, result.diagnostics);

    return result;
}

} // namespace scb
