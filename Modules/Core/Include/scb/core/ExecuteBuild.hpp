#pragma once

#include "scb/core/BuildPlan.hpp"

namespace scb {

enum class ActionStatus {
    Skipped,
    Executed,
    Failed
};

struct ActionExecution {
    std::string actionId;
    ActionStatus status = ActionStatus::Skipped;
    int exitCode = 0;
    std::string reason;
    std::string stdoutText;
    std::string stderrText;
};

struct BuildSummary {
    std::size_t executed = 0;
    std::size_t skipped = 0;
    std::size_t failed = 0;
    std::vector<ActionExecution> actions;
};

struct ExecuteBuildRequest {
    BuildPlan plan;
    bool dryRun = false;
    bool verbose = false;
    std::size_t jobs = 1;
};

struct ExecuteBuildResult {
    BuildSummary summary;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool ok() const;
};

[[nodiscard]] ExecuteBuildResult ExecuteBuild(const ExecuteBuildRequest& request);
[[nodiscard]] std::string ToString(ActionStatus status);

} // namespace scb
