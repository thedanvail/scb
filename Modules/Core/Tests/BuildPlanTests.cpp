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

    if (toolchain.toolchain.family == scb::ToolchainFamily::Gcc ||
        toolchain.toolchain.family == scb::ToolchainFamily::Clang) {
        REQUIRE(std::any_of(compile.command.args.begin(), compile.command.args.end(),
            [](const std::string& arg) { return arg.starts_with("-std="); }));
        REQUIRE(std::none_of(link.command.args.begin(), link.command.args.end(),
            [](const std::string& arg) { return arg.starts_with("-std="); }));
    }
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

TEST_CASE("planner builds shared library actions", "[plan]")
{
    const auto root = MakeTempRoot("plan_shared_library");
    WriteFile(root / "src" / "shared.cpp");

    const auto toolchain = scb::DetectHostToolchain({root, std::nullopt});
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
    INFO(Diagnostics(plan.diagnostics));
    REQUIRE(plan.ok());
    REQUIRE(plan.plan.actions.size() == 2);

    const auto& compile = FindAction(plan, scb::ActionKind::Compile, "shared");
    const auto& link = FindAction(plan, scb::ActionKind::Link, "shared");
    REQUIRE(compile.outputs.front().path.relative.find("target/debug/shared-lib/") != std::string::npos);

    if (toolchain.toolchain.family == scb::ToolchainFamily::Gcc ||
        toolchain.toolchain.family == scb::ToolchainFamily::Clang) {
        REQUIRE(std::find(compile.command.args.begin(), compile.command.args.end(), "-fPIC") != compile.command.args.end());
        REQUIRE(std::find(link.command.args.begin(), link.command.args.end(), "-shared") != link.command.args.end());
    }
}

TEST_CASE("planner orders static libraries dependent before dependency", "[plan]")
{
    const auto root = MakeTempRoot("plan_library_order");
    WriteFile(root / "src" / "main.cpp");
    WriteFile(root / "src" / "front.cpp");
    WriteFile(root / "src" / "back.cpp");

    const auto toolchain = scb::DetectHostToolchain({root, std::nullopt});
    REQUIRE(toolchain.ok());

    auto request = Request(root, toolchain.toolchain);
    request.hasManifest = true;
    request.manifest.project.name = "ordered";

    scb::ManifestTarget app;
    app.name = "ordered";
    app.kind = scb::TargetKind::Executable;
    app.sources.include = {"src/main.cpp"};
    app.dependencies = {{"lib:front"}};

    scb::ManifestTarget front;
    front.name = "front";
    front.kind = scb::TargetKind::StaticLibrary;
    front.sources.include = {"src/front.cpp"};
    front.dependencies = {{"lib:back"}};

    scb::ManifestTarget back;
    back.name = "back";
    back.kind = scb::TargetKind::StaticLibrary;
    back.sources.include = {"src/back.cpp"};

    request.manifest.targets = {app, front, back};

    const auto resolved = scb::ResolveProject(request);
    REQUIRE(resolved.ok());

    const auto plan = scb::PlanBuild({resolved.project});
    INFO(Diagnostics(plan.diagnostics));
    REQUIRE(plan.ok());

    const auto& link = FindAction(plan, scb::ActionKind::Link, "ordered");
    std::size_t frontIndex = std::string::npos;
    std::size_t backIndex = std::string::npos;
    for (std::size_t index = 0; index < link.inputs.size(); ++index) {
        const auto& relative = link.inputs[index].path.relative;
        if (relative.find("/front/") != std::string::npos) {
            frontIndex = index;
        }
        if (relative.find("/back/") != std::string::npos) {
            backIndex = index;
        }
    }

    REQUIRE(frontIndex != std::string::npos);
    REQUIRE(backIndex != std::string::npos);
    REQUIRE(frontIndex < backIndex);
}

TEST_CASE("planner emits source-dependencies for msvc compiles", "[plan]")
{
    const auto root = MakeTempRoot("plan_msvc_source_dependencies");
    WriteFile(root / "src" / "main.cpp");

    scb::ResolveRequest request;
    request.projectRoot = root;
    request.hasManifest = true;
    request.manifest.project.name = "msvc";

    scb::ManifestTarget target;
    target.name = "msvc";
    target.kind = scb::TargetKind::Executable;
    target.sources.include = {"src/main.cpp"};
    request.manifest.targets = {target};

    request.toolchain.family = scb::ToolchainFamily::Msvc;
    request.toolchain.compilerPath = "cl.exe";

    const auto resolved = scb::ResolveProject(request);
    REQUIRE(resolved.ok());

    const auto plan = scb::PlanBuild({resolved.project});
    INFO(Diagnostics(plan.diagnostics));
    REQUIRE(plan.ok());
    REQUIRE(plan.plan.actions.size() == 2);

    const auto& compile = FindAction(plan, scb::ActionKind::Compile, "msvc");
    REQUIRE(compile.depfile.has_value());
    REQUIRE(compile.depfileFormat == scb::DepfileFormat::SourceDependencies);
    REQUIRE(compile.depfile->relative.ends_with(".d.json"));

    bool foundSourceDependencies = false;
    for (std::size_t index = 0; index + 1 < compile.command.args.size(); ++index) {
        if (compile.command.args[index] == "/sourceDependencies" &&
            compile.command.args[index + 1] == compile.depfile->absolute.string()) {
            foundSourceDependencies = true;
            break;
        }
    }
    REQUIRE(foundSourceDependencies);
}
