#pragma once

#include "scb/core/ResolvedProject.hpp"

#include <optional>

namespace scb {

struct DetectToolchainRequest {
    std::filesystem::path projectRoot;
    std::optional<std::string> compilerOverride;
};

struct DetectToolchainResult {
    ToolchainInfo toolchain;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool ok() const;
};

[[nodiscard]] DetectToolchainResult DetectHostToolchain(const DetectToolchainRequest& request);

} // namespace scb
