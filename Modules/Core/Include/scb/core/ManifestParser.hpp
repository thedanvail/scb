#pragma once

#include "scb/core/ResolvedProject.hpp"

namespace scb {

struct ManifestParseRequest {
    std::filesystem::path manifestPath;
};

struct ManifestParseResult {
    ProjectManifest manifest;
    std::filesystem::path manifestPath;
    std::filesystem::path projectRoot;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool ok() const;
};

[[nodiscard]] ManifestParseResult ParseManifestFile(const ManifestParseRequest& request);
[[nodiscard]] std::string FormatDiagnostic(const Diagnostic& diagnostic);
[[nodiscard]] std::optional<std::string> GetCompilerOverride(const ProjectManifest& manifest);

} // namespace scb
