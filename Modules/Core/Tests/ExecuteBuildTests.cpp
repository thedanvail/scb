#include "scb/core/BuildPlan.hpp"
#include "scb/core/ExecuteBuild.hpp"
#include "scb/core/ResolvedProject.hpp"
#include "scb/core/Toolchain.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::filesystem::path MakeTempRoot(const std::string& name)
{
    auto root = std::filesystem::temp_directory_path() / ("scb_execute_build_tests_" + name);
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

void WriteFile(const std::filesystem::path& path, const std::string& contents)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path);
    stream << contents;
}

void BumpTimestamp(const std::filesystem::path& path)
{
    const auto current = std::filesystem::last_write_time(path);
    std::filesystem::last_write_time(path, current + std::chrono::seconds(2));
}

std::string Diagnostics(const std::vector<scb::Diagnostic>& diagnostics)
{
    std::ostringstream stream;
    for (const auto& diagnostic : diagnostics) {
        stream << diagnostic.message << '\n';
    }
    return stream.str();
}

scb::ResolveRequest Request(const std::filesystem::path& root, const scb::ToolchainInfo& toolchain)
{
    scb::ResolveRequest request;
    request.projectRoot = root;
    request.toolchain = toolchain;
    return request;
}

scb::ToolchainInfo RequireToolchain(const std::filesystem::path& root)
{
    const auto detected = scb::DetectHostToolchain({root, std::nullopt});
    REQUIRE(detected.ok());
    return detected.toolchain;
}

const scb::ActionNode& FindPlannedAction(const scb::BuildPlan& plan, scb::ActionKind kind, const std::string& ownerName)
{
    for (const auto& action : plan.actions) {
        if (action.kind == kind && action.owner.name == ownerName) {
            return action;
        }
    }
    FAIL("planned action not found");
}

bool HasExecutionReason(const scb::ExecuteBuildResult& result, std::string_view reason)
{
    for (const auto& action : result.summary.actions) {
        if (action.reason == reason) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("executor validates dependency ordering", "[execute]")
{
    scb::BuildPlan plan;
    plan.project.root.absolute = std::filesystem::current_path();
    plan.project.root.relative = ".";

    scb::ActionNode first;
    first.id = "first";
    first.kind = scb::ActionKind::Link;
    first.dependencies = {"second"};
    first.command.program = "/does/not/matter";

    scb::ActionNode second;
    second.id = "second";
    second.kind = scb::ActionKind::Link;
    second.command.program = "/does/not/matter";

    plan.actions = {first, second};

    const auto result = scb::ExecuteBuild({plan});
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.summary.actions.empty());
}

TEST_CASE("executor stops after first failing action", "[execute]")
{
    const auto root = MakeTempRoot("fail_fast");

    scb::BuildPlan plan;
    plan.project.root.absolute = root;
    plan.project.root.relative = ".";
    plan.project.toolchain.family = scb::ToolchainFamily::Gcc;

    scb::ActionNode first;
    first.id = "first";
    first.kind = scb::ActionKind::Link;
    first.label = "Broken action";
    first.outputs.push_back({{root / "out" / "first", "out/first"}});
    first.command.program = (root / "missing-program").string();

    scb::ActionNode second;
    second.id = "second";
    second.kind = scb::ActionKind::Link;
    second.label = "Never runs";
    second.outputs.push_back({{root / "out" / "second", "out/second"}});
    second.command.program = (root / "missing-program-2").string();

    plan.actions = {first, second};

    const auto result = scb::ExecuteBuild({plan});
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.summary.failed == 1);
    REQUIRE(result.summary.actions.size() == 1);
    REQUIRE(result.summary.actions.front().status == scb::ActionStatus::Failed);
}

#ifndef _WIN32
TEST_CASE("executor drains large stdout and stderr without deadlock", "[execute]")
{
    const auto root = MakeTempRoot("large_output");

    scb::BuildPlan plan;
    plan.project.root.absolute = root;
    plan.project.root.relative = ".";
    plan.project.toolchain.family = scb::ToolchainFamily::Gcc;

    scb::ActionNode action;
    action.id = "large-output";
    action.kind = scb::ActionKind::Link;
    action.label = "Large output";
    action.outputs.push_back({{root / "out" / "large-output", "out/large-output"}});
    action.stateFile = {root / "target" / "debug" / ".scb" / "actions" / "large-output.toml", "target/debug/.scb/actions/large-output.toml"};
    action.command.program = "/bin/sh";
    action.command.workingDirectory = root;
    action.command.args = {
        "-c",
        "i=0; while [ $i -lt 100000 ]; do printf x; printf y >&2; i=$((i + 1)); done; mkdir -p out; printf done > out/large-output",
    };
    action.signature.actionId = action.id;
    action.signature.kind = action.kind;
    action.signature.ownerTarget = "exe:large-output";
    action.signature.program = action.command.program;
    action.signature.args = action.command.args;
    action.signature.workingDirectory = root.string();
    action.signature.toolchainFamily = "gcc";
    action.signature.toolchainVersion = "test";
    action.signature.explicitInputs = {"synthetic"};
    action.signature.declaredOutputs = {"out/large-output"};

    plan.actions = {action};

    const auto result = scb::ExecuteBuild({plan});
    INFO(Diagnostics(result.diagnostics));
    REQUIRE(result.ok());
    REQUIRE(result.summary.executed == 1);
    REQUIRE(result.summary.actions.size() == 1);
    REQUIRE(result.summary.actions.front().stdoutText.size() >= 100000);
    REQUIRE(result.summary.actions.front().stderrText.size() >= 100000);
    REQUIRE(std::filesystem::exists(root / "out" / "large-output"));
}
#endif

TEST_CASE("executor builds and skips zero-config executable", "[execute]")
{
    const auto root = MakeTempRoot("build_and_skip");
    WriteFile(root / "src" / "main.cpp", "int main() { return 0; }\n");

    const auto toolchain = RequireToolchain(root);
    const auto firstResolved = scb::ResolveProject(Request(root, toolchain));
    REQUIRE(firstResolved.ok());
    const auto firstPlan = scb::PlanBuild({firstResolved.project});
    REQUIRE(firstPlan.ok());
    const auto firstBuild = scb::ExecuteBuild({firstPlan.plan});
    INFO(Diagnostics(firstBuild.diagnostics));
    REQUIRE(firstBuild.ok());
    REQUIRE(firstBuild.summary.executed == 2);

    if (toolchain.family == scb::ToolchainFamily::Msvc) {
        SKIP("MSVC is conservative in this milestone and always recompiles.");
    }

    const auto secondResolved = scb::ResolveProject(Request(root, toolchain));
    REQUIRE(secondResolved.ok());
    const auto secondPlan = scb::PlanBuild({secondResolved.project});
    REQUIRE(secondPlan.ok());
    const auto secondBuild = scb::ExecuteBuild({secondPlan.plan});
    INFO(Diagnostics(secondBuild.diagnostics));
    REQUIRE(secondBuild.ok());
    REQUIRE(secondBuild.summary.executed == 0);
    REQUIRE(secondBuild.summary.skipped == 2);
}

TEST_CASE("header changes rebuild affected executable", "[execute]")
{
    const auto root = MakeTempRoot("header_rebuild");
    WriteFile(root / "include" / "value.hpp", "#pragma once\ninline int value() { return 1; }\n");
    WriteFile(root / "src" / "main.cpp", "#include \"value.hpp\"\nint main() { return value(); }\n");

    const auto toolchain = RequireToolchain(root);
    if (toolchain.family == scb::ToolchainFamily::Msvc) {
        SKIP("MSVC depfile-backed incremental rebuilds are deferred.");
    }

    auto request = Request(root, toolchain);
    request.hasManifest = true;
    request.manifest.project.name = "header-app";

    scb::ManifestTarget app;
    app.name = "header-app";
    app.kind = scb::TargetKind::Executable;
    app.sources.include = {"src/main.cpp"};
    app.includeDirs = {"include"};
    request.manifest.targets = {app};

    const auto firstResolved = scb::ResolveProject(request);
    REQUIRE(firstResolved.ok());
    const auto firstPlan = scb::PlanBuild({firstResolved.project});
    REQUIRE(firstPlan.ok());
    const auto firstBuild = scb::ExecuteBuild({firstPlan.plan});
    INFO(Diagnostics(firstBuild.diagnostics));
    REQUIRE(firstBuild.ok());

    const auto header = root / "include" / "value.hpp";
    WriteFile(header, "#pragma once\ninline int value() { return 2; }\n");
    BumpTimestamp(header);

    const auto secondResolved = scb::ResolveProject(request);
    REQUIRE(secondResolved.ok());
    const auto secondPlan = scb::PlanBuild({secondResolved.project});
    REQUIRE(secondPlan.ok());
    const auto secondBuild = scb::ExecuteBuild({secondPlan.plan});
    INFO(Diagnostics(secondBuild.diagnostics));
    REQUIRE(secondBuild.ok());
    REQUIRE(secondBuild.summary.executed == 2);
    REQUIRE(secondBuild.summary.skipped == 0);
}

TEST_CASE("library source change rebuilds archive and dependent link", "[execute]")
{
    const auto root = MakeTempRoot("library_rebuild");
    WriteFile(root / "src" / "main.cpp", "int answer(); int main() { return answer(); }\n");
    WriteFile(root / "src" / "lib.cpp", "int answer() { return 1; }\n");
    WriteFile(root / "src" / "feature.cpp", "int feature() { return 2; }\n");

    const auto toolchain = RequireToolchain(root);
    if (toolchain.family == scb::ToolchainFamily::Msvc) {
        SKIP("MSVC depfile-backed incremental rebuilds are deferred.");
    }

    const auto firstResolved = scb::ResolveProject(Request(root, toolchain));
    REQUIRE(firstResolved.ok());
    const auto firstPlan = scb::PlanBuild({firstResolved.project});
    REQUIRE(firstPlan.ok());
    const auto firstBuild = scb::ExecuteBuild({firstPlan.plan});
    INFO(Diagnostics(firstBuild.diagnostics));
    REQUIRE(firstBuild.ok());
    REQUIRE(firstBuild.summary.executed == 5);

    const auto changedSource = root / "src" / "feature.cpp";
    WriteFile(changedSource, "int feature() { return 3; }\n");
    BumpTimestamp(changedSource);

    const auto secondResolved = scb::ResolveProject(Request(root, toolchain));
    REQUIRE(secondResolved.ok());
    const auto secondPlan = scb::PlanBuild({secondResolved.project});
    REQUIRE(secondPlan.ok());
    const auto secondBuild = scb::ExecuteBuild({secondPlan.plan});
    INFO(Diagnostics(secondBuild.diagnostics));
    REQUIRE(secondBuild.ok());
    REQUIRE(secondBuild.summary.executed == 3);
    REQUIRE(secondBuild.summary.skipped == 2);
}

TEST_CASE("dry run evaluates without writing artifacts", "[execute]")
{
    const auto root = MakeTempRoot("dry_run");
    WriteFile(root / "src" / "main.cpp", "int main() { return 0; }\n");

    const auto toolchain = RequireToolchain(root);
    const auto resolved = scb::ResolveProject(Request(root, toolchain));
    REQUIRE(resolved.ok());
    const auto plan = scb::PlanBuild({resolved.project});
    REQUIRE(plan.ok());
    const auto dryRun = scb::ExecuteBuild({plan.plan, true, false});
    INFO(Diagnostics(dryRun.diagnostics));
    REQUIRE(dryRun.ok());
    REQUIRE(dryRun.summary.executed == 2);
    REQUIRE_FALSE(std::filesystem::exists(root / "target"));
}

TEST_CASE("signature changes rebuild executable without source edits", "[execute]")
{
    const auto root = MakeTempRoot("signature_rebuild");
    WriteFile(root / "src" / "main.cpp", "#ifndef VALUE\n#define VALUE 0\n#endif\nint main() { return VALUE; }\n");

    const auto toolchain = RequireToolchain(root);
    if (toolchain.family == scb::ToolchainFamily::Msvc) {
        SKIP("MSVC is conservative in this milestone and always recompiles.");
    }

    auto request = Request(root, toolchain);
    request.hasManifest = true;
    request.manifest.project.name = "signature-app";

    scb::ManifestTarget app;
    app.name = "signature-app";
    app.kind = scb::TargetKind::Executable;
    app.sources.include = {"src/main.cpp"};
    request.manifest.targets = {app};

    const auto firstResolved = scb::ResolveProject(request);
    REQUIRE(firstResolved.ok());
    const auto firstPlan = scb::PlanBuild({firstResolved.project});
    REQUIRE(firstPlan.ok());
    const auto firstBuild = scb::ExecuteBuild({firstPlan.plan});
    INFO(Diagnostics(firstBuild.diagnostics));
    REQUIRE(firstBuild.ok());
    REQUIRE(firstBuild.summary.executed == 2);

    request.manifest.targets.front().defines = {{"VALUE", "7"}};

    const auto secondResolved = scb::ResolveProject(request);
    REQUIRE(secondResolved.ok());
    const auto secondPlan = scb::PlanBuild({secondResolved.project});
    REQUIRE(secondPlan.ok());
    const auto secondBuild = scb::ExecuteBuild({secondPlan.plan});
    INFO(Diagnostics(secondBuild.diagnostics));
    REQUIRE(secondBuild.ok());
    REQUIRE(secondBuild.summary.executed == 2);
    REQUIRE(secondBuild.summary.skipped == 0);
    REQUIRE(HasExecutionReason(secondBuild, "action signature changed"));
}

TEST_CASE("missing action state triggers conservative rebuild", "[execute]")
{
    const auto root = MakeTempRoot("missing_action_state");
    WriteFile(root / "src" / "main.cpp", "int main() { return 0; }\n");

    const auto toolchain = RequireToolchain(root);
    if (toolchain.family == scb::ToolchainFamily::Msvc) {
        SKIP("MSVC is conservative in this milestone and always recompiles.");
    }

    const auto firstResolved = scb::ResolveProject(Request(root, toolchain));
    REQUIRE(firstResolved.ok());
    const auto firstPlan = scb::PlanBuild({firstResolved.project});
    REQUIRE(firstPlan.ok());
    const auto firstBuild = scb::ExecuteBuild({firstPlan.plan});
    INFO(Diagnostics(firstBuild.diagnostics));
    REQUIRE(firstBuild.ok());

    const auto& compileAction = FindPlannedAction(firstPlan.plan, scb::ActionKind::Compile, root.filename().string());
    std::filesystem::remove(compileAction.stateFile.absolute);

    const auto secondResolved = scb::ResolveProject(Request(root, toolchain));
    REQUIRE(secondResolved.ok());
    const auto secondPlan = scb::PlanBuild({secondResolved.project});
    REQUIRE(secondPlan.ok());
    const auto secondBuild = scb::ExecuteBuild({secondPlan.plan});
    INFO(Diagnostics(secondBuild.diagnostics));
    REQUIRE(secondBuild.ok());
    REQUIRE(secondBuild.summary.executed == 2);
    REQUIRE(secondBuild.summary.skipped == 0);
    REQUIRE(HasExecutionReason(secondBuild, "action state missing: " + compileAction.stateFile.relative));
}

TEST_CASE("corrupt action state triggers conservative rebuild", "[execute]")
{
    const auto root = MakeTempRoot("corrupt_action_state");
    WriteFile(root / "src" / "main.cpp", "int main() { return 0; }\n");

    const auto toolchain = RequireToolchain(root);
    if (toolchain.family == scb::ToolchainFamily::Msvc) {
        SKIP("MSVC is conservative in this milestone and always recompiles.");
    }

    const auto firstResolved = scb::ResolveProject(Request(root, toolchain));
    REQUIRE(firstResolved.ok());
    const auto firstPlan = scb::PlanBuild({firstResolved.project});
    REQUIRE(firstPlan.ok());
    const auto firstBuild = scb::ExecuteBuild({firstPlan.plan});
    INFO(Diagnostics(firstBuild.diagnostics));
    REQUIRE(firstBuild.ok());

    const auto& compileAction = FindPlannedAction(firstPlan.plan, scb::ActionKind::Compile, root.filename().string());
    WriteFile(compileAction.stateFile.absolute, "not = [valid");

    const auto secondResolved = scb::ResolveProject(Request(root, toolchain));
    REQUIRE(secondResolved.ok());
    const auto secondPlan = scb::PlanBuild({secondResolved.project});
    REQUIRE(secondPlan.ok());
    const auto secondBuild = scb::ExecuteBuild({secondPlan.plan});
    INFO(Diagnostics(secondBuild.diagnostics));
    REQUIRE(secondBuild.ok());
    REQUIRE(secondBuild.summary.executed == 2);
    REQUIRE(secondBuild.summary.skipped == 0);
    REQUIRE(HasExecutionReason(secondBuild, "action state unreadable: " + compileAction.stateFile.relative));
}
