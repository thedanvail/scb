#pragma once

#include <filesystem>
#include <string>

namespace scb {

struct ProjectPath {
    std::filesystem::path absolute;
    std::string relative;

    [[nodiscard]] bool operator==(const ProjectPath&) const = default;
};

} // namespace scb
