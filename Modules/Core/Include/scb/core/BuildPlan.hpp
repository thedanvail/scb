#pragma once

#include "scb/core/ResolvedProject.hpp"

namespace scb {

enum class ActionKind {
    Compile,
    Archive,
    Link
};

enum class DepfileFormat {
    None,
    GnuMake,
    SourceDependencies
};

struct PlannedArtifact {
    ProjectPath path;
};

struct CommandLine {
    std::string program;
    std::vector<std::string> args;
    std::optional<std::filesystem::path> workingDirectory;
};

struct ActionSignature {
    std::string actionId;
    ActionKind kind = ActionKind::Compile;
    std::string ownerTarget;
    std::string program;
    std::vector<std::string> args;
    std::string workingDirectory;
    std::string toolchainFamily;
    std::string toolchainVersion;
    DepfileFormat depfileFormat = DepfileFormat::None;
    std::optional<std::string> depfilePath;
    std::vector<std::string> explicitInputs;
    std::vector<std::string> declaredOutputs;

    [[nodiscard]] bool operator==(const ActionSignature&) const = default;
};

struct ActionState {
    ActionSignature signature;
    std::vector<std::string> discoveredInputs;

    [[nodiscard]] bool operator==(const ActionState&) const = default;
};

struct ActionNode {
    std::string id;
    ActionKind kind = ActionKind::Compile;
    TargetId owner;
    std::string label;
    std::vector<PlannedArtifact> inputs;
    std::vector<PlannedArtifact> outputs;
    std::optional<ProjectPath> depfile;
    DepfileFormat depfileFormat = DepfileFormat::None;
    ProjectPath stateFile;
    std::vector<std::string> dependencies;
    CommandLine command;
    ActionSignature signature;
};

struct BuildPlan {
    ResolvedProject project;
    std::vector<ActionNode> actions;
};

struct PlanBuildRequest {
    ResolvedProject project;
};

struct PlanBuildResult {
    BuildPlan plan;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool ok() const;
};

[[nodiscard]] PlanBuildResult PlanBuild(const PlanBuildRequest& request);
[[nodiscard]] std::string ToString(ActionKind kind);
[[nodiscard]] std::string ToString(DepfileFormat format);

[[nodiscard]] std::string ToJson(const BuildPlan& plan);

} // namespace scb
