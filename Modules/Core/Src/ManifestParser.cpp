#include "scb/core/ManifestParser.hpp"

#include <toml.hpp>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <optional>
#include <set>
#include <sstream>

namespace scb {
namespace {

using TomlValue = toml::value;
using TomlTable = TomlValue::table_type;
using TomlArray = TomlValue::array_type;

[[nodiscard]] bool HasErrors(const std::vector<Diagnostic>& diagnostics)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::Error;
    });
}

[[nodiscard]] std::optional<Diagnostic::Location> MakeLocation(const toml::source_location& location)
{
    if (!location.is_ok()) {
        return std::nullopt;
    }
    return Diagnostic::Location{
        std::filesystem::path(location.file_name()),
        location.first_line_number(),
        location.first_column_number(),
    };
}

void AddError(std::vector<Diagnostic>& diagnostics, std::string message, const toml::source_location& location)
{
    diagnostics.push_back({DiagnosticSeverity::Error, std::move(message), MakeLocation(location)});
}

void AddError(std::vector<Diagnostic>& diagnostics, std::string message)
{
    diagnostics.push_back({DiagnosticSeverity::Error, std::move(message), std::nullopt});
}

void ValidateAllowedKeys(
    const TomlTable& table,
    std::initializer_list<std::string_view> allowedKeys,
    std::string_view context,
    std::vector<Diagnostic>& diagnostics)
{
    for (const auto& [key, value] : table) {
        const bool allowed = std::find(allowedKeys.begin(), allowedKeys.end(), key) != allowedKeys.end();
        if (!allowed) {
            AddError(diagnostics, std::string(context) + " contains unknown key '" + key + "'", value.location());
        }
    }
}

[[nodiscard]] std::optional<std::string> ExpectString(
    const TomlValue& value,
    std::string_view context,
    std::vector<Diagnostic>& diagnostics)
{
    if (!value.is_string()) {
        AddError(diagnostics, std::string(context) + " must be a string", value.location());
        return std::nullopt;
    }
    return value.as_string();
}

[[nodiscard]] std::optional<bool> ExpectBoolean(
    const TomlValue& value,
    std::string_view context,
    std::vector<Diagnostic>& diagnostics)
{
    if (!value.is_boolean()) {
        AddError(diagnostics, std::string(context) + " must be a boolean", value.location());
        return std::nullopt;
    }
    return value.as_boolean();
}

[[nodiscard]] std::vector<std::string> ParseStringArray(
    const TomlValue& value,
    std::string_view context,
    std::vector<Diagnostic>& diagnostics)
{
    std::vector<std::string> strings;
    if (!value.is_array()) {
        AddError(diagnostics, std::string(context) + " must be an array of strings", value.location());
        return strings;
    }

    for (const auto& element : value.as_array()) {
        if (auto string = ExpectString(element, std::string(context) + " element", diagnostics)) {
            strings.push_back(*string);
        }
    }
    return strings;
}

[[nodiscard]] std::optional<std::string> ParseDefineValue(
    const TomlValue& value,
    std::string_view context,
    std::vector<Diagnostic>& diagnostics)
{
    if (value.is_string()) {
        return value.as_string();
    }
    if (value.is_integer()) {
        return std::to_string(value.as_integer());
    }
    if (value.is_boolean()) {
        return value.as_boolean() ? "true" : "false";
    }
    if (value.is_floating()) {
        std::ostringstream stream;
        stream << value.as_floating();
        return stream.str();
    }

    AddError(diagnostics, std::string(context) + " must be a string, integer, boolean, or floating-point value", value.location());
    return std::nullopt;
}

[[nodiscard]] std::map<std::string, std::string> ParseDefines(
    const TomlValue& value,
    std::string_view context,
    std::vector<Diagnostic>& diagnostics)
{
    std::map<std::string, std::string> defines;
    if (!value.is_table()) {
        AddError(diagnostics, std::string(context) + " must be a table", value.location());
        return defines;
    }

    for (const auto& [key, entry] : value.as_table()) {
        if (auto parsed = ParseDefineValue(entry, std::string(context) + "." + key, diagnostics)) {
            defines[key] = *parsed;
        }
    }
    return defines;
}

void ParseBuildOptions(
    ManifestBuildOptions& options,
    const TomlTable& table,
    std::string_view context,
    bool allowStandard,
    std::vector<Diagnostic>& diagnostics)
{
    if (allowStandard) {
        ValidateAllowedKeys(
            table,
            {"include_dirs", "defines", "compile_flags", "link_flags", "standard", "optimization", "debug_info"},
            context,
            diagnostics);
    } else {
        ValidateAllowedKeys(
            table,
            {"include_dirs", "defines", "compile_flags", "link_flags", "optimization", "debug_info"},
            context,
            diagnostics);
    }

    if (auto it = table.find("include_dirs"); it != table.end()) {
        options.includeDirs = ParseStringArray(it->second, std::string(context) + ".include_dirs", diagnostics);
    }
    if (auto it = table.find("defines"); it != table.end()) {
        options.defines = ParseDefines(it->second, std::string(context) + ".defines", diagnostics);
    }
    if (auto it = table.find("compile_flags"); it != table.end()) {
        options.compileFlags = ParseStringArray(it->second, std::string(context) + ".compile_flags", diagnostics);
    }
    if (auto it = table.find("link_flags"); it != table.end()) {
        options.linkFlags = ParseStringArray(it->second, std::string(context) + ".link_flags", diagnostics);
    }
    if (allowStandard) {
        if (auto it = table.find("standard"); it != table.end()) {
            if (auto value = ExpectString(it->second, std::string(context) + ".standard", diagnostics)) {
                options.standard = *value;
            }
        }
    }
    if (auto it = table.find("optimization"); it != table.end()) {
        if (auto value = ExpectString(it->second, std::string(context) + ".optimization", diagnostics)) {
            options.optimization = *value;
        }
    }
    if (auto it = table.find("debug_info"); it != table.end()) {
        if (auto value = ExpectBoolean(it->second, std::string(context) + ".debug_info", diagnostics)) {
            options.hasDebugInfo = true;
            options.debugInfo = *value;
        }
    }
}

[[nodiscard]] std::optional<TargetKind> ParseTargetKind(
    const TomlValue& value,
    std::string_view context,
    std::vector<Diagnostic>& diagnostics)
{
    const auto string = ExpectString(value, context, diagnostics);
    if (!string.has_value()) {
        return std::nullopt;
    }

    if (*string == "exe") {
        return TargetKind::Executable;
    }
    if (*string == "test") {
        return TargetKind::TestExecutable;
    }
    if (*string == "static-lib" || *string == "lib") {
        return TargetKind::StaticLibrary;
    }
    if (*string == "shared-lib") {
        return TargetKind::SharedLibrary;
    }
    if (*string == "header-lib" || *string == "header-only") {
        return TargetKind::HeaderOnly;
    }

    AddError(diagnostics, std::string(context) + " has unsupported target kind '" + *string + "'", value.location());
    return std::nullopt;
}

void ParseTargetSources(
    ManifestSourceSet& sources,
    const TomlValue& value,
    std::string_view context,
    std::vector<Diagnostic>& diagnostics)
{
    if (!value.is_table()) {
        AddError(diagnostics, std::string(context) + " must be a table", value.location());
        return;
    }

    const auto& table = value.as_table();
    ValidateAllowedKeys(table, {"include", "exclude"}, context, diagnostics);

    if (auto it = table.find("include"); it != table.end()) {
        sources.include = ParseStringArray(it->second, std::string(context) + ".include", diagnostics);
    }
    if (auto it = table.find("exclude"); it != table.end()) {
        sources.exclude = ParseStringArray(it->second, std::string(context) + ".exclude", diagnostics);
    }
}

[[nodiscard]] ManifestTarget ParseTarget(
    const TomlValue& value,
    std::size_t index,
    std::vector<Diagnostic>& diagnostics)
{
    ManifestTarget target;
    if (!value.is_table()) {
        AddError(diagnostics, "target[" + std::to_string(index) + "] must be a table", value.location());
        return target;
    }

    const auto& table = value.as_table();
    ValidateAllowedKeys(
        table,
        {"name", "kind", "sources", "include_dirs", "defines", "compile_flags", "link_flags", "standard", "deps"},
        "target[" + std::to_string(index) + "]",
        diagnostics);

    if (auto it = table.find("name"); it != table.end()) {
        if (auto name = ExpectString(it->second, "target[" + std::to_string(index) + "].name", diagnostics)) {
            target.name = *name;
        }
    } else {
        AddError(diagnostics, "target[" + std::to_string(index) + "] is missing required key 'name'", value.location());
    }

    if (auto it = table.find("kind"); it != table.end()) {
        if (auto kind = ParseTargetKind(it->second, "target[" + std::to_string(index) + "].kind", diagnostics)) {
            target.kind = *kind;
        }
    } else {
        AddError(diagnostics, "target[" + std::to_string(index) + "] is missing required key 'kind'", value.location());
    }

    if (auto it = table.find("sources"); it != table.end()) {
        ParseTargetSources(target.sources, it->second, "target[" + std::to_string(index) + "].sources", diagnostics);
    }
    if (auto it = table.find("include_dirs"); it != table.end()) {
        target.includeDirs = ParseStringArray(it->second, "target[" + std::to_string(index) + "].include_dirs", diagnostics);
    }
    if (auto it = table.find("defines"); it != table.end()) {
        target.defines = ParseDefines(it->second, "target[" + std::to_string(index) + "].defines", diagnostics);
    }
    if (auto it = table.find("compile_flags"); it != table.end()) {
        target.compileFlags = ParseStringArray(it->second, "target[" + std::to_string(index) + "].compile_flags", diagnostics);
    }
    if (auto it = table.find("link_flags"); it != table.end()) {
        target.linkFlags = ParseStringArray(it->second, "target[" + std::to_string(index) + "].link_flags", diagnostics);
    }
    if (auto it = table.find("standard"); it != table.end()) {
        if (auto standard = ExpectString(it->second, "target[" + std::to_string(index) + "].standard", diagnostics)) {
            target.standard = *standard;
        }
    }
    if (auto it = table.find("deps"); it != table.end()) {
        for (const auto& dependency : ParseStringArray(it->second, "target[" + std::to_string(index) + "].deps", diagnostics)) {
            target.dependencies.push_back({dependency});
        }
    }

    return target;
}

void ParseProfiles(
    std::map<std::string, ManifestProfile>& profiles,
    const TomlValue& value,
    std::vector<Diagnostic>& diagnostics)
{
    if (!value.is_table()) {
        AddError(diagnostics, "profile must be a table", value.location());
        return;
    }

    for (const auto& [name, entry] : value.as_table()) {
        if (!entry.is_table()) {
            AddError(diagnostics, "profile." + name + " must be a table", entry.location());
            continue;
        }
        ManifestProfile profile;
        profile.name = name;
        ParseBuildOptions(profile.build, entry.as_table(), "profile." + name, true, diagnostics);
        profiles[name] = std::move(profile);
    }
}

void ParseProjectInfo(ProjectManifest& manifest, const TomlValue& value, std::vector<Diagnostic>& diagnostics)
{
    if (!value.is_table()) {
        AddError(diagnostics, "project must be a table", value.location());
        return;
    }

    const auto& table = value.as_table();
    ValidateAllowedKeys(table, {"name", "version", "standard"}, "project", diagnostics);

    if (auto it = table.find("name"); it != table.end()) {
        if (auto name = ExpectString(it->second, "project.name", diagnostics)) {
            manifest.project.name = *name;
        }
    }
    if (auto it = table.find("version"); it != table.end()) {
        if (auto version = ExpectString(it->second, "project.version", diagnostics)) {
            manifest.project.version = *version;
        }
    }
    if (auto it = table.find("standard"); it != table.end()) {
        if (auto standard = ExpectString(it->second, "project.standard", diagnostics)) {
            manifest.project.standard = *standard;
        }
    }
}

void ParseToolchain(ProjectManifest& manifest, const TomlValue& value, std::vector<Diagnostic>& diagnostics)
{
    if (!value.is_table()) {
        AddError(diagnostics, "toolchain must be a table", value.location());
        return;
    }

    const auto& table = value.as_table();
    ValidateAllowedKeys(table, {"compiler"}, "toolchain", diagnostics);

    if (auto it = table.find("compiler"); it != table.end()) {
        if (auto compiler = ExpectString(it->second, "toolchain.compiler", diagnostics)) {
            manifest.toolchain.compiler = *compiler;
        }
    }
}

[[nodiscard]] ProjectManifest ParseManifestRoot(const TomlValue& root, std::vector<Diagnostic>& diagnostics)
{
    ProjectManifest manifest;

    if (!root.is_table()) {
        AddError(diagnostics, "manifest root must be a table", root.location());
        return manifest;
    }

    const auto& table = root.as_table();
    ValidateAllowedKeys(table, {"project", "build", "toolchain", "profile", "target"}, "manifest", diagnostics);

    if (auto it = table.find("project"); it != table.end()) {
        ParseProjectInfo(manifest, it->second, diagnostics);
    }
    if (auto it = table.find("build"); it != table.end()) {
        if (!it->second.is_table()) {
            AddError(diagnostics, "build must be a table", it->second.location());
        } else {
            ParseBuildOptions(manifest.build, it->second.as_table(), "build", false, diagnostics);
        }
    }
    if (auto it = table.find("toolchain"); it != table.end()) {
        ParseToolchain(manifest, it->second, diagnostics);
    }
    if (auto it = table.find("profile"); it != table.end()) {
        ParseProfiles(manifest.profiles, it->second, diagnostics);
    }
    if (auto it = table.find("target"); it != table.end()) {
        if (!it->second.is_array()) {
            AddError(diagnostics, "target must be an array of tables", it->second.location());
        } else {
            const auto& targets = it->second.as_array();
            for (std::size_t index = 0; index < targets.size(); ++index) {
                manifest.targets.push_back(ParseTarget(targets[index], index, diagnostics));
            }
        }
    }

    return manifest;
}

} // namespace

bool ManifestParseResult::ok() const
{
    return !HasErrors(diagnostics);
}

ManifestParseResult ParseManifestFile(const ManifestParseRequest& request)
{
    ManifestParseResult result;
    result.manifestPath = std::filesystem::absolute(request.manifestPath).lexically_normal();
    result.projectRoot = result.manifestPath.parent_path();

    const auto parsed = toml::try_parse(result.manifestPath, toml::spec::v(1, 0, 0));
    if (parsed.is_err()) {
        for (const auto& error : parsed.as_err()) {
            if (error.locations().empty()) {
                result.diagnostics.push_back({DiagnosticSeverity::Error, toml::format_error(error), std::nullopt});
                continue;
            }

            const auto& location = error.locations().front().first;
            result.diagnostics.push_back({
                DiagnosticSeverity::Error,
                toml::format_error(error),
                MakeLocation(location),
            });
        }
        return result;
    }

    result.manifest = ParseManifestRoot(parsed.as_ok(), result.diagnostics);
    return result;
}

std::string FormatDiagnostic(const Diagnostic& diagnostic)
{
    std::ostringstream stream;
    stream << (diagnostic.severity == DiagnosticSeverity::Error ? "error" : "warning");
    if (diagnostic.location.has_value()) {
        stream << ": " << diagnostic.location->file.string();
        if (diagnostic.location->line > 0) {
            stream << ':' << diagnostic.location->line;
            if (diagnostic.location->column > 0) {
                stream << ':' << diagnostic.location->column;
            }
        }
    }
    stream << ": " << diagnostic.message;
    return stream.str();
}

std::optional<std::string> GetCompilerOverride(const ProjectManifest& manifest)
{
    if (manifest.toolchain.compiler.empty()) {
        return std::nullopt;
    }
    return manifest.toolchain.compiler;
}

} // namespace scb
