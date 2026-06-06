#include "scb/core/ResolvedProject.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::filesystem::path MakeTempRoot(const std::string& name)
{
    auto root = std::filesystem::temp_directory_path() / ("scb_resolver_tests_" + name);
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

std::string Diagnostics(const scb::ResolveResult& result)
{
    std::ostringstream stream;
    for (const auto& diagnostic : result.diagnostics) {
        stream << diagnostic.message << '\n';
    }
    return stream.str();
}

const scb::ResolvedTarget& FindTarget(const scb::ResolveResult& result, scb::TargetKind kind, const std::string& name)
{
    for (const auto& target : result.project.targets) {
        if (target.id.kind == kind && target.id.name == name) {
            return target;
        }
    }
    FAIL("target not found: " + scb::ToString(kind) + ":" + name + "\n" + Diagnostics(result));
}

scb::ResolveRequest Request(const std::filesystem::path& root)
{
    scb::ResolveRequest request;
    request.projectRoot = root;
    request.toolchain.family = scb::ToolchainFamily::Gcc;
    request.toolchain.compilerPath = "g++";
    request.toolchain.version = "test";
    return request;
}

} // namespace

TEST_CASE("resolver infers a zero-config executable", "[resolver]")
{
    const auto root = MakeTempRoot("zero_config_executable");
    WriteFile(root / "src" / "main.cpp");

    auto result = scb::ResolveProject(Request(root));

    REQUIRE(result.ok());
    INFO(Diagnostics(result));
    REQUIRE(result.project.name == root.filename().string());
    REQUIRE(result.project.targets.size() == 1);

    const auto& target = FindTarget(result, scb::TargetKind::Executable, root.filename().string());
    REQUIRE(target.sources.compileSources.size() == 1);
    REQUIRE(target.sources.compileSources.front().path.relative == "src/main.cpp");
    REQUIRE(target.dependencies.empty());
}

TEST_CASE("resolver infers library plus executable for zero-config project", "[resolver]")
{
    const auto root = MakeTempRoot("zero_config_library_and_executable");
    WriteFile(root / "src" / "main.cpp");
    WriteFile(root / "src" / "lib.cpp");
    WriteFile(root / "src" / "feature.cpp");

    auto result = scb::ResolveProject(Request(root));

    REQUIRE(result.ok());
    INFO(Diagnostics(result));
    REQUIRE(result.project.targets.size() == 2);

    const auto& library = FindTarget(result, scb::TargetKind::StaticLibrary, root.filename().string());
    REQUIRE(library.sources.compileSources.size() == 2);
    REQUIRE(library.sources.compileSources[0].path.relative == "src/feature.cpp");
    REQUIRE(library.sources.compileSources[1].path.relative == "src/lib.cpp");

    const auto& executable = FindTarget(result, scb::TargetKind::Executable, root.filename().string());
    REQUIRE(executable.dependencies.size() == 1);
    REQUIRE(executable.dependencies.front().kind == scb::TargetKind::StaticLibrary);
    REQUIRE(executable.dependencies.front().name == root.filename().string());
}

TEST_CASE("resolver infers a header-only library from include directory", "[resolver]")
{
    const auto root = MakeTempRoot("header_only_zero_config");
    WriteFile(root / "include" / "lib.hpp");

    auto result = scb::ResolveProject(Request(root));

    REQUIRE(result.ok());
    INFO(Diagnostics(result));
    const auto& target = FindTarget(result, scb::TargetKind::HeaderOnly, root.filename().string());
    REQUIRE(target.sources.headers.size() == 1);
    REQUIRE(target.sources.compileSources.empty());
    REQUIRE(target.build.includeDirs.size() == 1);
    REQUIRE(target.build.includeDirs.front() == "include");
}

TEST_CASE("explicit manifest targets suppress zero-config discovery", "[resolver]")
{
    const auto root = MakeTempRoot("explicit_targets_suppress_discovery");
    WriteFile(root / "src" / "main.cpp");
    WriteFile(root / "tool" / "tool.cpp");

    auto request = Request(root);
    request.hasManifest = true;
    request.manifest.name = "explicit";

    scb::ManifestTarget target;
    target.name = "tool";
    target.kind = scb::TargetKind::Executable;
    target.sources.include = {"tool/tool.cpp"};
    request.manifest.targets.push_back(target);

    auto result = scb::ResolveProject(request);

    REQUIRE(result.ok());
    INFO(Diagnostics(result));
    REQUIRE(result.project.targets.size() == 1);
    static_cast<void>(FindTarget(result, scb::TargetKind::Executable, "tool"));
}

TEST_CASE("resolver expands globs and sorts results deterministically", "[resolver]")
{
    const auto root = MakeTempRoot("glob_expansion_and_sorting");
    WriteFile(root / "src" / "z.cpp");
    WriteFile(root / "src" / "a.cpp");
    WriteFile(root / "src" / "ignored.cpp");

    auto request = Request(root);
    request.hasManifest = true;
    request.manifest.name = "glob";

    scb::ManifestTarget target;
    target.name = "glob";
    target.kind = scb::TargetKind::StaticLibrary;
    target.sources.include = {"src/**/*.cpp"};
    target.sources.exclude = {"src/ignored.cpp"};
    request.manifest.targets.push_back(target);

    auto result = scb::ResolveProject(request);

    REQUIRE(result.ok());
    INFO(Diagnostics(result));
    const auto& resolved = FindTarget(result, scb::TargetKind::StaticLibrary, "glob");
    REQUIRE(resolved.sources.compileSources.size() == 2);
    REQUIRE(resolved.sources.compileSources[0].path.relative == "src/a.cpp");
    REQUIRE(resolved.sources.compileSources[1].path.relative == "src/z.cpp");
}

TEST_CASE("resolver applies build option precedence", "[resolver]")
{
    const auto root = MakeTempRoot("build_option_precedence");
    WriteFile(root / "src" / "main.cpp");
    std::filesystem::create_directories(root / "include");
    std::filesystem::create_directories(root / "profile_include");
    std::filesystem::create_directories(root / "target_include");

    auto request = Request(root);
    request.hasManifest = true;
    request.manifest.name = "options";
    request.profile = "release";
    request.manifest.build.standard = "c++17";
    request.manifest.build.includeDirs = {"include"};
    request.manifest.build.defines = {{"MODE", "project"}, {"KEEP", "1"}};

    scb::ManifestProfile profile;
    profile.name = "release";
    profile.build.standard = "c++20";
    profile.build.includeDirs = {"profile_include"};
    profile.build.defines = {{"MODE", "profile"}};
    profile.build.hasDebugInfo = true;
    profile.build.debugInfo = false;
    request.manifest.profiles["release"] = profile;

    scb::ManifestTarget target;
    target.name = "options";
    target.kind = scb::TargetKind::Executable;
    target.sources.include = {"src/main.cpp"};
    target.includeDirs = {"target_include"};
    target.standard = "c++23";
    target.defines = {{"MODE", "target"}};
    request.manifest.targets.push_back(target);

    auto result = scb::ResolveProject(request);

    REQUIRE(result.ok());
    INFO(Diagnostics(result));
    const auto& resolved = FindTarget(result, scb::TargetKind::Executable, "options");
    REQUIRE(resolved.build.standard == scb::CxxStandard::Cxx23);
    REQUIRE(resolved.build.optimization == scb::OptimizationLevel::Speed);
    REQUIRE_FALSE(resolved.build.debugInfo);
    REQUIRE(resolved.build.defines.at("MODE") == "target");
    REQUIRE(resolved.build.defines.at("KEEP") == "1");
    REQUIRE(resolved.build.includeDirs.size() == 3);
    REQUIRE(resolved.build.includeDirs[0] == "include");
    REQUIRE(resolved.build.includeDirs[1] == "profile_include");
    REQUIRE(resolved.build.includeDirs[2] == "target_include");
}

TEST_CASE("resolver rejects invalid manifest inputs", "[resolver]")
{
    const auto root = MakeTempRoot("validation_errors");
    WriteFile(root / "src" / "main.cpp");

    SECTION("absolute source paths fail")
    {
        auto request = Request(root);
        request.hasManifest = true;
        request.manifest.name = "bad";

        scb::ManifestTarget target;
        target.name = "bad";
        target.kind = scb::TargetKind::Executable;
        target.sources.include = {(root / "src" / "main.cpp").string()};
        request.manifest.targets.push_back(target);

        auto result = scb::ResolveProject(request);
        INFO(Diagnostics(result));
        REQUIRE_FALSE(result.ok());
    }

    SECTION("duplicate targets fail")
    {
        auto request = Request(root);
        request.hasManifest = true;
        request.manifest.name = "bad";

        scb::ManifestTarget target;
        target.name = "dup";
        target.kind = scb::TargetKind::StaticLibrary;
        target.sources.include = {"src/main.cpp"};
        request.manifest.targets = {target, target};

        auto result = scb::ResolveProject(request);
        INFO(Diagnostics(result));
        REQUIRE_FALSE(result.ok());
    }

    SECTION("unknown dependencies fail")
    {
        auto request = Request(root);
        request.hasManifest = true;
        request.manifest.name = "bad";

        scb::ManifestTarget target;
        target.name = "app";
        target.kind = scb::TargetKind::Executable;
        target.sources.include = {"src/main.cpp"};
        target.dependencies = {{"missing"}};
        request.manifest.targets.push_back(target);

        auto result = scb::ResolveProject(request);
        INFO(Diagnostics(result));
        REQUIRE_FALSE(result.ok());
    }
}

TEST_CASE("resolver rejects dependency cycles", "[resolver]")
{
    const auto root = MakeTempRoot("dependency_cycle");
    WriteFile(root / "src" / "a.cpp");
    WriteFile(root / "src" / "b.cpp");

    auto request = Request(root);
    request.hasManifest = true;
    request.manifest.name = "cycle";

    scb::ManifestTarget a;
    a.name = "a";
    a.kind = scb::TargetKind::StaticLibrary;
    a.sources.include = {"src/a.cpp"};
    a.dependencies = {{"lib:b"}};

    scb::ManifestTarget b;
    b.name = "b";
    b.kind = scb::TargetKind::StaticLibrary;
    b.sources.include = {"src/b.cpp"};
    b.dependencies = {{"lib:a"}};

    request.manifest.targets = {a, b};

    auto result = scb::ResolveProject(request);
    INFO(Diagnostics(result));
    REQUIRE_FALSE(result.ok());
}
