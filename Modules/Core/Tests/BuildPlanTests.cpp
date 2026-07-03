#include "scb/core/BuildPlan.hpp"
#include "scb/core/ResolvedProject.hpp"
#include "scb/core/Toolchain.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::filesystem::path MakeTempRoot(const std::string& name)
{
    auto root = std::filesystem::temp_directory_path() / ("scb_build_plan_tests_" + name);
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

void WriteFile(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path);
    stream << "int value = 1;\n";
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

const scb::ActionNode& FindAction(const scb::PlanBuildResult& result, scb::ActionKind kind, const std::string& ownerName)
{
    for (const auto& action : result.plan.actions) {
        if (action.kind == kind && action.owner.name == ownerName) {
            return action;
        }
    }
    FAIL("action not found");
}

} // namespace

TEST_CASE("toolchain detection finds a host compiler", "[toolchain]")
{
    const auto root = MakeTempRoot("detect_host_compiler");
    const auto result = scb::DetectHostToolchain({root, std::nullopt});

    INFO(Diagnostics(result.diagnostics));
    REQUIRE(result.ok());
    REQUIRE(!result.toolchain.compilerPath.empty());
    REQUIRE(result.toolchain.family != scb::ToolchainFamily::Unknown);
}

TEST_CASE("toolchain detection rejects missing compiler override", "[toolchain]")
{
    const auto root = MakeTempRoot("missing_override");
    const auto result = scb::DetectHostToolchain({root, std::string("toolchains/does-not-exist++")});

    REQUIRE_FALSE(result.ok());
}

TEST_CASE("planner builds compile and link actions for executable", "[plan]")
{
    const auto root = MakeTempRoot("plan_executable");
    WriteFile(root / "src" / "main.cpp");

    const auto toolchain = scb::DetectHostToolchain({root, std::nullopt});
    REQUIRE(toolchain.ok());

    const auto resolved = scb::ResolveProject(Request(root, toolchain.toolchain));
    REQUIRE(resolved.ok());

    const auto plan = scb::PlanBuild({resolved.project});
    INFO(Diagnostics(plan.diagnostics));
    REQUIRE(plan.ok());
    REQUIRE(plan.plan.actions.size() == 2);

    const auto& compile = FindAction(plan, scb::ActionKind::Compile, root.filename().string());
    const auto& link = FindAction(plan, scb::ActionKind::Link, root.filename().string());
    REQUIRE(compile.outputs.front().path.relative.find("target/debug/exe/") != std::string::npos);
    REQUIRE(compile.depfile.has_value());
    REQUIRE(compile.depfileFormat == scb::DepfileFormat::GnuMake);
    REQUIRE(link.dependencies.size() == 1);
}

TEST_CASE("planner builds archive and link actions for library dependency", "[plan]")
{
    const auto root = MakeTempRoot("plan_library_dependency");
    WriteFile(root / "src" / "main.cpp");
    WriteFile(root / "src" / "lib.cpp");
    WriteFile(root / "src" / "feature.cpp");

    const auto toolchain = scb::DetectHostToolchain({root, std::nullopt});
    REQUIRE(toolchain.ok());

    const auto resolved = scb::ResolveProject(Request(root, toolchain.toolchain));
    REQUIRE(resolved.ok());

    const auto plan = scb::PlanBuild({resolved.project});
    INFO(Diagnostics(plan.diagnostics));
    REQUIRE(plan.ok());
    REQUIRE(plan.plan.actions.size() == 5);

    const auto& archive = FindAction(plan, scb::ActionKind::Archive, root.filename().string());
    const auto& link = FindAction(plan, scb::ActionKind::Link, root.filename().string());
    REQUIRE_FALSE(archive.command.program.empty());
    REQUIRE(link.dependencies.size() >= 2);
}

TEST_CASE("planner emits no actions for header-only dependency", "[plan]")
{
    const auto root = MakeTempRoot("plan_header_only_dependency");
    WriteFile(root / "src" / "main.cpp");
    WriteFile(root / "include" / "lib.hpp");

    auto toolchain = scb::DetectHostToolchain({root, std::nullopt});
    REQUIRE(toolchain.ok());

    auto request = Request(root, toolchain.toolchain);
    request.hasManifest = true;
    request.manifest.project.name = "app";

    scb::ManifestTarget app;
    app.name = "app";
    app.kind = scb::TargetKind::Executable;
    app.sources.include = {"src/main.cpp"};
    app.dependencies = {{"header"}};

    scb::ManifestTarget header;
    header.name = "header";
    header.kind = scb::TargetKind::HeaderOnly;
    header.sources.include = {"include/**/*.hpp"};
    header.includeDirs = {"include"};

    request.manifest.targets = {app, header};

    const auto resolved = scb::ResolveProject(request);
    REQUIRE(resolved.ok());

    const auto plan = scb::PlanBuild({resolved.project});
    INFO(Diagnostics(plan.diagnostics));
    REQUIRE(plan.ok());
    REQUIRE(plan.plan.actions.size() == 2);
}

TEST_CASE("planner rejects shared libraries for now", "[plan]")
{
    const auto root = MakeTempRoot("plan_shared_library");
    WriteFile(root / "src" / "shared.cpp");

    auto toolchain = scb::DetectHostToolchain({root, std::nullopt});
    REQUIRE(toolchain.ok());

    auto request = Request(root, toolchain.toolchain);
    request.hasManifest = true;
    request.manifest.project.name = "shared";

    scb::ManifestTarget shared;
    shared.name = "shared";
    shared.kind = scb::TargetKind::SharedLibrary;
    shared.sources.include = {"src/shared.cpp"};
    request.manifest.targets = {shared};

    const auto resolved = scb::ResolveProject(request);
    REQUIRE(resolved.ok());

    const auto plan = scb::PlanBuild({resolved.project});
    REQUIRE_FALSE(plan.ok());
}
