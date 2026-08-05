#include "scb/core/CleanProject.hpp"

#include <algorithm>
#include <optional>
#include <set>

namespace scb {

namespace {

[[nodiscard]] std::optional<std::filesystem::path> ResolveRoot(const std::filesystem::path& raw)
{
    auto normalized = raw.lexically_normal();
    if (normalized.is_relative()) {
        normalized = std::filesystem::absolute(normalized).lexically_normal();
    }
    if (!std::filesystem::exists(normalized) || !std::filesystem::is_directory(normalized)) {
        return std::nullopt;
    }
    return normalized;
}

[[nodiscard]] bool RemoveDirectory(const std::filesystem::path& path, std::vector<Diagnostic>& diagnostics)
{
    std::error_code error;
    if (!std::filesystem::exists(path, error) || !std::filesystem::is_directory(path, error)) {
        return true;
    }

    const auto removed = std::filesystem::remove_all(path, error);
    if (error) {
        diagnostics.push_back({
            DiagnosticSeverity::Error,
            "failed to remove " + path.string() + ": " + error.message(),
            std::nullopt,
        });
        return false;
    }

    if (removed > 0) {
        return true;
    }

    diagnostics.push_back({
        DiagnosticSeverity::Warning,
        "directory was not removed: " + path.string(),
        std::nullopt,
    });
    return false;
}

} // namespace

bool CleanResult::ok() const
{
    return std::none_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::Error;
    });
}

CleanResult CleanProject(const CleanRequest& request)
{
    CleanResult result;
    const auto root = ResolveRoot(request.projectRoot);
    if (!root.has_value()) {
        result.diagnostics.push_back({
            DiagnosticSeverity::Error,
            "project root does not exist: " + request.projectRoot.string(),
            std::nullopt,
        });
        return result;
    }

    const auto targetRoot = *root / "target";
    if (!std::filesystem::exists(targetRoot)) {
        result.cleaned = true;
        return result;
    }

    std::set<std::filesystem::path> removed;
    if (request.allProfiles) {
        if (RemoveDirectory(targetRoot, result.diagnostics)) {
            removed.insert(targetRoot);
        }
    } else if (!request.profile.empty()) {
        const auto profileDirectory = targetRoot / request.profile;
        if (RemoveDirectory(profileDirectory, result.diagnostics)) {
            removed.insert(profileDirectory);
        }
    } else {
        result.diagnostics.push_back({
            DiagnosticSeverity::Error,
            "clean requires a profile or --all",
            std::nullopt,
        });
        return result;
    }

    result.removedDirectories.assign(removed.begin(), removed.end());
    result.cleaned = result.ok();
    return result;
}

} // namespace scb
