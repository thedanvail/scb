#pragma once

#include <map>
#include <string>
#include <vector>

namespace scb {

enum class CxxStandard {
    Cxx17,
    Cxx20,
    Cxx23
};

enum class OptimizationLevel {
    None,
    Debug,
    Speed,
    Size
};

struct ManifestBuildOptions {
    std::vector<std::string> includeDirs;
    std::map<std::string, std::string> defines;
    std::vector<std::string> compileFlags;
    std::vector<std::string> linkFlags;
    std::string standard;
    std::string optimization;
    bool hasDebugInfo = false;
    bool debugInfo = false;
};

struct ResolvedBuildOptions {
    std::vector<std::string> includeDirs;
    std::map<std::string, std::string> defines;
    std::vector<std::string> compileFlags;
    std::vector<std::string> linkFlags;
    CxxStandard standard = CxxStandard::Cxx20;
    OptimizationLevel optimization = OptimizationLevel::Debug;
    bool debugInfo = true;
};

} // namespace scb
