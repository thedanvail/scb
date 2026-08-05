#pragma once

#include "scb/core/ResolvedProject.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace scb {

[[nodiscard]] std::vector<std::filesystem::path> ParseGnuMakeDepfile(
    const std::filesystem::path& depfile,
    const std::filesystem::path& workingDirectory,
    std::vector<Diagnostic>& diagnostics);

[[nodiscard]] std::vector<std::filesystem::path> ParseSourceDependenciesFile(
    const std::filesystem::path& depfile,
    const std::filesystem::path& workingDirectory,
    std::vector<Diagnostic>& diagnostics);

} // namespace scb
