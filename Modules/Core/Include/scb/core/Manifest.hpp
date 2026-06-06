#pragma once

#include "scb/core/BuildOptions.hpp"

#include <map>
#include <string>
#include <vector>

namespace scb {

enum class TargetKind {
    Executable,
    StaticLibrary,
    SharedLibrary,
    HeaderOnly
};

struct ManifestSourceSet {
    std::vector<std::string> include;
    std::vector<std::string> exclude;
};

struct ManifestDependency {
    std::string value;
};

struct ManifestTarget {
    std::string name;
    TargetKind kind = TargetKind::Executable;
    ManifestSourceSet sources;
    std::vector<std::string> includeDirs;
    std::map<std::string, std::string> defines;
    std::vector<std::string> compileFlags;
    std::vector<std::string> linkFlags;
    std::string standard;
    std::vector<ManifestDependency> dependencies;
};

struct ManifestProfile {
    std::string name;
    ManifestBuildOptions build;
};

struct ProjectManifest {
    std::string name;
    std::string version;
    ManifestBuildOptions build;
    std::vector<ManifestTarget> targets;
    std::map<std::string, ManifestProfile> profiles;
};

} // namespace scb
