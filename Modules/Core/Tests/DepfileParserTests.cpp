#include "scb/core/DepfileParser.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::filesystem::path MakeTempRoot(const std::string& name)
{
    auto root = std::filesystem::temp_directory_path() / ("scb_depfile_tests_" + name);
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

void WriteFile(const std::filesystem::path& path, const std::string& contents)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << contents;
}

std::string Diagnostics(const std::vector<scb::Diagnostic>& diagnostics)
{
    std::ostringstream stream;
    for (const auto& diagnostic : diagnostics) {
        stream << diagnostic.message << '\n';
    }
    return stream.str();
}

} // namespace

TEST_CASE("source dependencies parser extracts include paths", "[depfile]")
{
    const auto root = MakeTempRoot("source_dependencies");
    const auto depfile = root / "source.dep.json";

    WriteFile(depfile, R"json({
  "Version": "1.1",
  "Data": {
    "Source": "/tmp/scb_dep_abs/src/main.cpp",
    "Includes": [
      "/tmp/scb_dep_abs/include/a.hpp",
      "include/b.hpp"
    ]
  }
})json");

    std::vector<scb::Diagnostic> diagnostics;
    auto dependencies = scb::ParseSourceDependenciesFile(depfile, root, diagnostics);

    INFO(Diagnostics(diagnostics));
    REQUIRE(dependencies.size() == 2);
    REQUIRE(dependencies[0] == std::filesystem::path("/tmp/scb_dep_abs/include/a.hpp"));
    REQUIRE(dependencies[1] == (root / "include" / "b.hpp").lexically_normal());
}

TEST_CASE("source dependencies parser handles escaped strings", "[depfile]")
{
    const auto root = MakeTempRoot("source_dependencies_escaped");
    const auto depfile = root / "source.dep.json";

    WriteFile(depfile, R"json({
  "Data": {
    "Includes": [
      "/tmp/scb_dep_abs/include/quote\".hpp",
      "include/back/slash.hpp"
    ]
  }
})json");

    std::vector<scb::Diagnostic> diagnostics;
    auto dependencies = scb::ParseSourceDependenciesFile(depfile, root, diagnostics);

    INFO(Diagnostics(diagnostics));
    REQUIRE(dependencies.size() == 2);
    REQUIRE(dependencies[0] == std::filesystem::path("/tmp/scb_dep_abs/include/quote\".hpp"));
    REQUIRE(dependencies[1] == (root / "include" / "back" / "slash.hpp").lexically_normal());
}

TEST_CASE("source dependencies parser skips byte order mark", "[depfile]")
{
    const auto root = MakeTempRoot("source_dependencies_bom");
    const auto depfile = root / "source.dep.json";

    const std::string bom = "\xEF\xBB\xBF";
    WriteFile(depfile, bom + R"json({
  "Data": {
    "Includes": ["include/x.hpp"]
  }
})json");

    std::vector<scb::Diagnostic> diagnostics;
    auto dependencies = scb::ParseSourceDependenciesFile(depfile, root, diagnostics);

    INFO(Diagnostics(diagnostics));
    REQUIRE(dependencies.size() == 1);
    REQUIRE(dependencies[0] == (root / "include" / "x.hpp").lexically_normal());
}

TEST_CASE("source dependencies parser treats missing Includes as empty", "[depfile]")
{
    const auto root = MakeTempRoot("source_dependencies_missing");
    const auto depfile = root / "source.dep.json";

    WriteFile(depfile, R"json({
  "Data": {
    "Source": "src/main.cpp"
  }
})json");

    std::vector<scb::Diagnostic> diagnostics;
    auto dependencies = scb::ParseSourceDependenciesFile(depfile, root, diagnostics);

    INFO(Diagnostics(diagnostics));
    REQUIRE(dependencies.empty());
}

TEST_CASE("source dependencies parser reports empty file", "[depfile]")
{
    const auto root = MakeTempRoot("source_dependencies_empty");
    const auto depfile = root / "source.dep.json";

    WriteFile(depfile, "");

    std::vector<scb::Diagnostic> diagnostics;
    auto dependencies = scb::ParseSourceDependenciesFile(depfile, root, diagnostics);

    REQUIRE(dependencies.empty());
    REQUIRE_FALSE(diagnostics.empty());
}
