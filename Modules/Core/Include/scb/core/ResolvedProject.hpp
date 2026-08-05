#pragma once

#include "scb/core/BuildOptions.hpp"
#include "scb/core/Manifest.hpp"
#include "scb/core/Path.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace scb {

enum class ToolchainFamily {
    Unknown,
    Gcc,
    Clang,
    Msvc
};

enum class DiagnosticSeverity {
    Error,
    Warning
};

struct Diagnostic {
    struct Location {
        std::filesystem::path file;
        std::size_t line = 0;
        std::size_t column = 0;
    };

    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string message;
    std::optional<Location> location;
};

struct SourceFile {
    ProjectPath path;
};

struct SourceSet {
    std::vector<SourceFile> compileSources;
    std::vector<SourceFile> headers;
};

struct TargetId {
    TargetKind kind = TargetKind::Executable;
    std::string name;
};

enum class TargetOrigin {
    Explicit,
    Inferred
};

struct ResolvedTarget {
    TargetId id;
    TargetOrigin origin = TargetOrigin::Explicit;
    SourceSet sources;
    ResolvedBuildOptions build;
    std::vector<TargetId> dependencies;
    ProjectPath artifactDirectory;
};

struct ResolvedProfile {
    std::string name;
    ResolvedBuildOptions build;
};

struct ToolchainInfo {
    ToolchainFamily family = ToolchainFamily::Unknown;
    std::string compilerPath;
    std::string archiverPath;
    std::string linkerPath;
    std::string version;
    std::string identity;
};

struct ResolvedProject {
    std::string name;
    ProjectPath root;
    ResolvedProfile profile;
    ToolchainInfo toolchain;
    std::vector<ResolvedTarget> targets;
};

struct ResolveRequest {
    std::filesystem::path projectRoot;
    ProjectManifest manifest;
    bool hasManifest = false;
    std::string profile = "debug";
    ToolchainInfo toolchain;
};

struct ResolveResult {
    ResolvedProject project;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool ok() const;
};

[[nodiscard]] ResolveResult ResolveProject(const ResolveRequest& request);

[[nodiscard]] std::string ToString(TargetKind kind);
[[nodiscard]] std::string ToString(CxxStandard standard);
[[nodiscard]] std::string ToString(OptimizationLevel optimization);
[[nodiscard]] std::string ToString(ToolchainFamily family);
[[nodiscard]] std::string ToString(TargetOrigin origin);

} // namespace scb
