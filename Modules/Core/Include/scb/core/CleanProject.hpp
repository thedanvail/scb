#pragma once

#include "scb/core/ResolvedProject.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace scb {

struct CleanRequest {
    std::filesystem::path projectRoot;
    std::string profile;
    bool allProfiles = false;
};

struct CleanResult {
    bool cleaned = false;
    std::vector<std::filesystem::path> removedDirectories;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool ok() const;
};

[[nodiscard]] CleanResult CleanProject(const CleanRequest& request);

} // namespace scb
