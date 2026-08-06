#include "scb/core/DepfileParser.hpp"
#include "scb/core/ExecuteBuild.hpp"

#include <toml.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>

#ifndef _WIN32
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#define NOMINMAX
#include <windows.h>
#endif

namespace scb {
namespace {

struct ProcessResult {
    int exitCode = 0;
    std::string stdoutText;
    std::string stderrText;
};

struct DirtyCheckResult {
    bool dirty = true;
    std::string reason;
    std::vector<Diagnostic> diagnostics;
};

struct ActionStateLoadResult {
    bool found = false;
    bool valid = false;
    ActionState state;
    std::string reason;
};

using TomlValue = toml::value;
using TomlTable = TomlValue::table_type;
using TomlArray = TomlValue::array_type;

[[nodiscard]] bool HasErrors(const std::vector<Diagnostic>& diagnostics)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::Error;
    });
}

[[nodiscard]] std::string DisplayPath(const ProjectPath& path)
{
    return path.relative.empty() ? path.absolute.string() : path.relative;
}

[[nodiscard]] std::string CommandString(const CommandLine& command)
{
    std::ostringstream stream;
    stream << command.program;
    for (const auto& arg : command.args) {
        stream << ' ' << arg;
    }
    return stream.str();
}

void EnsureParentDirectory(const std::filesystem::path& path, std::vector<Diagnostic>& diagnostics)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        diagnostics.push_back({
            DiagnosticSeverity::Error,
            "failed to create directory for output: " + path.parent_path().string(),
            std::nullopt,
        });
    }
}

[[nodiscard]] std::string StorePath(const std::filesystem::path& root, const std::filesystem::path& path)
{
    const auto normalizedRoot = root.lexically_normal();
    const auto normalizedPath = path.lexically_normal();
    if (normalizedPath.is_relative()) {
        return normalizedPath.generic_string();
    }

    const auto relative = normalizedPath.lexically_relative(normalizedRoot);
    if (!relative.empty()) {
        bool escapesRoot = false;
        for (const auto& part : relative) {
            if (part == "..") {
                escapesRoot = true;
                break;
            }
        }
        if (!escapesRoot) {
            return relative.generic_string();
        }
    }
    return normalizedPath.generic_string();
}

[[nodiscard]] std::filesystem::path LoadPath(const std::filesystem::path& root, const std::string& stored)
{
    std::filesystem::path path(stored);
    if (path.is_absolute()) {
        return path.lexically_normal();
    }
    return (root / path).lexically_normal();
}

[[nodiscard]] TomlArray ToTomlArray(const std::vector<std::string>& values)
{
    TomlArray array;
    array.reserve(values.size());
    for (const auto& value : values) {
        array.emplace_back(value);
    }
    return array;
}

[[nodiscard]] TomlArray ToTomlArray(const std::vector<FileFingerprint>& values)
{
    TomlArray array;
    array.reserve(values.size());
    for (const auto& value : values) {
        TomlTable table;
        table["path"] = value.path;
        table["digest"] = value.digest;
        table["size"] = static_cast<std::int64_t>(value.size);
        array.emplace_back(table);
    }
    return array;
}

[[nodiscard]] std::optional<std::vector<FileFingerprint>> ReadRequiredFingerprintArray(const TomlTable& table, std::string_view key)
{
    const auto it = table.find(std::string(key));
    if (it == table.end() || !it->second.is_array()) {
        return std::nullopt;
    }

    std::vector<FileFingerprint> values;
    for (const auto& entry : it->second.as_array()) {
        if (!entry.is_table()) {
            return std::nullopt;
        }
        const auto& fingerprintTable = entry.as_table();
        const auto pathIt = fingerprintTable.find("path");
        const auto digestIt = fingerprintTable.find("digest");
        const auto sizeIt = fingerprintTable.find("size");
        if (pathIt == fingerprintTable.end() || !pathIt->second.is_string() ||
            digestIt == fingerprintTable.end() || !digestIt->second.is_string() ||
            sizeIt == fingerprintTable.end() || !sizeIt->second.is_integer()) {
            return std::nullopt;
        }

        FileFingerprint fingerprint;
        fingerprint.path = pathIt->second.as_string();
        fingerprint.digest = digestIt->second.as_string();
        fingerprint.size = static_cast<std::uint64_t>(sizeIt->second.as_integer());
        values.push_back(std::move(fingerprint));
    }
    return values;
}

[[nodiscard]] std::optional<FileFingerprint> FingerprintFile(const std::filesystem::path& root, const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }

    std::uint64_t hash = 1469598103934665603ull;
    std::uint64_t size = 0;
    char buffer[8192];
    while (stream) {
        stream.read(buffer, sizeof(buffer));
        const std::streamsize count = stream.gcount();
        for (std::streamsize index = 0; index < count; ++index) {
            hash ^= static_cast<unsigned char>(buffer[index]);
            hash *= 1099511628211ull;
        }
        size += static_cast<std::uint64_t>(count);
    }

    if (!stream.eof()) {
        return std::nullopt;
    }

    std::ostringstream digest;
    digest << std::hex << std::setw(16) << std::setfill('0') << hash;

    FileFingerprint fingerprint;
    fingerprint.path = StorePath(root, path);
    fingerprint.digest = digest.str();
    fingerprint.size = size;
    return fingerprint;
}

[[nodiscard]] bool FingerprintMatches(const std::filesystem::path& root, const FileFingerprint& stored)
{
    const auto current = FingerprintFile(root, LoadPath(root, stored.path));
    return current.has_value() && *current == stored;
}

[[nodiscard]] std::optional<ActionKind> ActionKindFromString(std::string_view value)
{
    if (value == "compile") {
        return ActionKind::Compile;
    }
    if (value == "archive") {
        return ActionKind::Archive;
    }
    if (value == "link") {
        return ActionKind::Link;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<DepfileFormat> DepfileFormatFromString(std::string_view value)
{
    if (value == "none") {
        return DepfileFormat::None;
    }
    if (value == "gnu-make") {
        return DepfileFormat::GnuMake;
    }
    if (value == "source-dependencies") {
        return DepfileFormat::SourceDependencies;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> ReadRequiredString(const TomlTable& table, std::string_view key)
{
    const auto it = table.find(std::string(key));
    if (it == table.end() || !it->second.is_string()) {
        return std::nullopt;
    }
    return it->second.as_string();
}

[[nodiscard]] std::optional<std::vector<std::string>> ReadRequiredStringArray(const TomlTable& table, std::string_view key)
{
    const auto it = table.find(std::string(key));
    if (it == table.end() || !it->second.is_array()) {
        return std::nullopt;
    }

    std::vector<std::string> values;
    for (const auto& entry : it->second.as_array()) {
        if (!entry.is_string()) {
            return std::nullopt;
        }
        values.push_back(entry.as_string());
    }
    return values;
}

[[nodiscard]] std::optional<std::string> ReadOptionalString(const TomlTable& table, std::string_view key)
{
    const auto it = table.find(std::string(key));
    if (it == table.end()) {
        return std::nullopt;
    }
    if (!it->second.is_string()) {
        return std::nullopt;
    }
    return it->second.as_string();
}

ActionStateLoadResult LoadActionState(const BuildPlan& plan, const ActionNode& action)
{
    ActionStateLoadResult result;
    if (!std::filesystem::exists(action.stateFile.absolute)) {
        result.reason = "action state missing: " + action.stateFile.relative;
        return result;
    }

    const auto parsed = toml::try_parse(action.stateFile.absolute, toml::spec::v(1, 0, 0));
    if (parsed.is_err()) {
        result.found = true;
        result.reason = "action state unreadable: " + action.stateFile.relative;
        return result;
    }

    const auto& root = parsed.as_ok();
    if (!root.is_table()) {
        result.found = true;
        result.reason = "action state has invalid structure: " + action.stateFile.relative;
        return result;
    }

    const auto& table = root.as_table();
    ActionState state;
    state.signature.actionId = ReadRequiredString(table, "action_id").value_or("");
    const auto kind = ReadRequiredString(table, "action_kind");
    const auto depfileFormat = ReadRequiredString(table, "depfile_format");
    state.signature.ownerTarget = ReadRequiredString(table, "owner_target").value_or("");
    state.signature.program = ReadRequiredString(table, "program").value_or("");
    state.signature.args = ReadRequiredStringArray(table, "args").value_or(std::vector<std::string>{});
    state.signature.workingDirectory = ReadRequiredString(table, "working_directory").value_or("");
    state.signature.toolchainFamily = ReadRequiredString(table, "toolchain_family").value_or("");
    state.signature.toolchainVersion = ReadRequiredString(table, "toolchain_version").value_or("");
    state.signature.toolchainIdentity = ReadRequiredString(table, "toolchain_identity").value_or("");
    state.signature.depfilePath = ReadOptionalString(table, "depfile_path");
    state.signature.explicitInputs = ReadRequiredStringArray(table, "explicit_inputs").value_or(std::vector<std::string>{});
    state.signature.declaredOutputs = ReadRequiredStringArray(table, "declared_outputs").value_or(std::vector<std::string>{});
    auto explicitFingerprints = ReadRequiredFingerprintArray(table, "explicit_input_fingerprints");
    auto discoveredFingerprints = ReadRequiredFingerprintArray(table, "discovered_input_fingerprints");

    if (state.signature.actionId.empty() || state.signature.ownerTarget.empty() || state.signature.program.empty() ||
        state.signature.workingDirectory.empty() || state.signature.toolchainFamily.empty() ||
        state.signature.toolchainIdentity.empty() || !kind.has_value() || !depfileFormat.has_value() ||
        state.signature.declaredOutputs.empty() || !explicitFingerprints.has_value() || !discoveredFingerprints.has_value()) {
        result.found = true;
        result.reason = "action state is missing required fields: " + action.stateFile.relative;
        return result;
    }

    const auto parsedKind = ActionKindFromString(*kind);
    const auto parsedFormat = DepfileFormatFromString(*depfileFormat);
    if (!parsedKind.has_value() || !parsedFormat.has_value()) {
        result.found = true;
        result.reason = "action state contains invalid enum values: " + action.stateFile.relative;
        return result;
    }

    state.signature.kind = *parsedKind;
    state.signature.depfileFormat = *parsedFormat;
    state.explicitInputs = std::move(*explicitFingerprints);
    state.discoveredInputs = std::move(*discoveredFingerprints);

    result.found = true;
    result.valid = true;
    result.state = std::move(state);
    result.reason = "loaded action state";
    return result;
}

void WriteActionState(const ActionNode& action, const ActionState& state, std::vector<Diagnostic>& diagnostics)
{
    TomlTable table;
    table["action_id"] = state.signature.actionId;
    table["action_kind"] = ToString(state.signature.kind);
    table["owner_target"] = state.signature.ownerTarget;
    table["program"] = state.signature.program;
    table["args"] = ToTomlArray(state.signature.args);
    table["working_directory"] = state.signature.workingDirectory;
    table["toolchain_family"] = state.signature.toolchainFamily;
    table["toolchain_version"] = state.signature.toolchainVersion;
    table["toolchain_identity"] = state.signature.toolchainIdentity;
    table["depfile_format"] = ToString(state.signature.depfileFormat);
    if (state.signature.depfilePath.has_value()) {
        table["depfile_path"] = *state.signature.depfilePath;
    }
    table["explicit_inputs"] = ToTomlArray(state.signature.explicitInputs);
    table["declared_outputs"] = ToTomlArray(state.signature.declaredOutputs);
    table["explicit_input_fingerprints"] = ToTomlArray(state.explicitInputs);
    table["discovered_input_fingerprints"] = ToTomlArray(state.discoveredInputs);

    std::ofstream stream(action.stateFile.absolute);
    if (!stream) {
        diagnostics.push_back({
            DiagnosticSeverity::Warning,
            "failed to open action state for writing: " + action.stateFile.relative,
            std::nullopt,
        });
        return;
    }

    stream << toml::format(TomlValue(table));
    if (!stream) {
        diagnostics.push_back({
            DiagnosticSeverity::Warning,
            "failed to write action state: " + action.stateFile.relative,
            std::nullopt,
        });
    }
}

struct PreparedActionState {
    bool ok = true;
    ActionState state;
    std::vector<Diagnostic> diagnostics;
};

[[nodiscard]] std::vector<std::filesystem::path> ParseDepfileForAction(
    const BuildPlan& plan,
    const ActionNode& action,
    std::vector<Diagnostic>& diagnostics)
{
    const auto workingDirectory = action.command.workingDirectory.value_or(plan.project.root.absolute);
    switch (action.depfileFormat) {
    case DepfileFormat::GnuMake:
        return ParseGnuMakeDepfile(action.depfile->absolute, workingDirectory, diagnostics);
    case DepfileFormat::SourceDependencies:
        return ParseSourceDependenciesFile(action.depfile->absolute, workingDirectory, diagnostics);
    case DepfileFormat::None:
        return {};
    }
    return {};
}

[[nodiscard]] PreparedActionState PrepareActionState(const BuildPlan& plan, const ActionNode& action)
{
    PreparedActionState prepared;
    prepared.state.signature = action.signature;

    for (const auto& input : action.inputs) {
        auto fingerprint = FingerprintFile(plan.project.root.absolute, input.path.absolute);
        if (!fingerprint.has_value()) {
            prepared.ok = false;
            prepared.diagnostics.push_back({
                DiagnosticSeverity::Error,
                "failed to fingerprint explicit input: " + DisplayPath(input.path),
                std::nullopt,
            });
            return prepared;
        }
        prepared.state.explicitInputs.push_back(std::move(*fingerprint));
    }

    if (action.depfileFormat == DepfileFormat::None) {
        return prepared;
    }
    if (!action.depfile.has_value()) {
        prepared.ok = false;
        prepared.diagnostics.push_back({
            DiagnosticSeverity::Error,
            "action expected a depfile but none was declared: " + action.label,
            std::nullopt,
        });
        return prepared;
    }
    if (!std::filesystem::exists(action.depfile->absolute)) {
        prepared.ok = false;
        prepared.diagnostics.push_back({
            DiagnosticSeverity::Error,
            "compiler did not produce required depfile: " + action.depfile->relative,
            std::nullopt,
        });
        return prepared;
    }

    auto dependencies = ParseDepfileForAction(plan, action, prepared.diagnostics);
    if (dependencies.empty()) {
        prepared.ok = false;
        prepared.diagnostics.push_back({
            DiagnosticSeverity::Error,
            "compiler produced an unusable depfile: " + action.depfile->relative,
            std::nullopt,
        });
        return prepared;
    }

    std::set<std::string> seen;
    for (const auto& dependency : dependencies) {
        const auto storedPath = StorePath(plan.project.root.absolute, dependency);
        if (!seen.insert(storedPath).second) {
            continue;
        }
        auto fingerprint = FingerprintFile(plan.project.root.absolute, dependency);
        if (!fingerprint.has_value()) {
            prepared.ok = false;
            prepared.diagnostics.push_back({
                DiagnosticSeverity::Error,
                "failed to fingerprint discovered dependency: " + storedPath,
                std::nullopt,
            });
            return prepared;
        }
        prepared.state.discoveredInputs.push_back(std::move(*fingerprint));
    }
    return prepared;
}

[[nodiscard]] DirtyCheckResult CheckCompileDirty(const BuildPlan& plan, const ActionNode& action, const std::filesystem::path& workingDirectory, ToolchainFamily family)
{
    (void)family;
    (void)workingDirectory;
    DirtyCheckResult result;
    if (action.outputs.empty()) {
        result.dirty = true;
        result.reason = "action declares no outputs";
        return result;
    }

    for (const auto& output : action.outputs) {
        if (!std::filesystem::exists(output.path.absolute)) {
            result.dirty = true;
            result.reason = "output missing: " + DisplayPath(output.path);
            return result;
        }
    }

    const auto state = LoadActionState(plan, action);
    if (!state.found || !state.valid) {
        result.dirty = true;
        result.reason = state.reason;
        return result;
    }

    if (state.state.signature != action.signature) {
        result.dirty = true;
        result.reason = "action signature changed";
        return result;
    }

    for (const auto& input : action.inputs) {
        if (!std::filesystem::exists(input.path.absolute)) {
            result.dirty = true;
            result.reason = "explicit input missing: " + DisplayPath(input.path);
            return result;
        }
    }

    for (const auto& fingerprint : state.state.explicitInputs) {
        if (!FingerprintMatches(plan.project.root.absolute, fingerprint)) {
            result.dirty = true;
            result.reason = "explicit input changed: " + fingerprint.path;
            return result;
        }
    }

    for (const auto& fingerprint : state.state.discoveredInputs) {
        const auto dependency = LoadPath(plan.project.root.absolute, fingerprint.path);
        if (!std::filesystem::exists(dependency)) {
            result.dirty = true;
            result.reason = "discovered dependency missing: " + fingerprint.path;
            return result;
        }
        if (!FingerprintMatches(plan.project.root.absolute, fingerprint)) {
            result.dirty = true;
            result.reason = "discovered dependency changed: " + fingerprint.path;
            return result;
        }
    }

    result.dirty = false;
    result.reason = "up to date";
    return result;
}

[[nodiscard]] DirtyCheckResult CheckSimpleDirty(const BuildPlan& plan, const ActionNode& action)
{
    DirtyCheckResult result;
    if (action.outputs.empty()) {
        result.dirty = true;
        result.reason = "action declares no outputs";
        return result;
    }

    for (const auto& output : action.outputs) {
        if (!std::filesystem::exists(output.path.absolute)) {
            result.dirty = true;
            result.reason = "output missing: " + DisplayPath(output.path);
            return result;
        }
    }

    const auto state = LoadActionState(plan, action);
    if (!state.found || !state.valid) {
        result.dirty = true;
        result.reason = state.reason;
        return result;
    }

    if (state.state.signature != action.signature) {
        result.dirty = true;
        result.reason = "action signature changed";
        return result;
    }

    for (const auto& input : action.inputs) {
        if (!std::filesystem::exists(input.path.absolute)) {
            result.dirty = true;
            result.reason = "explicit input missing: " + DisplayPath(input.path);
            return result;
        }
    }

    for (const auto& fingerprint : state.state.explicitInputs) {
        if (!FingerprintMatches(plan.project.root.absolute, fingerprint)) {
            result.dirty = true;
            result.reason = "explicit input changed: " + fingerprint.path;
            return result;
        }
    }

    result.dirty = false;
    result.reason = "up to date";
    return result;
}

[[nodiscard]] DirtyCheckResult CheckDirty(const BuildPlan& plan, const ActionNode& action)
{
    const auto workingDirectory = action.command.workingDirectory.value_or(plan.project.root.absolute);
    switch (action.kind) {
    case ActionKind::Compile:
        return CheckCompileDirty(plan, action, workingDirectory, plan.project.toolchain.family);
    case ActionKind::Archive:
    case ActionKind::Link:
        return CheckSimpleDirty(plan, action);
    }
    return {};
}

#ifndef _WIN32
void AppendAvailablePipeOutput(int descriptor, std::string& output, bool& open)
{
    char buffer[4096];
    while (true) {
        const auto count = ::read(descriptor, buffer, sizeof(buffer));
        if (count > 0) {
            output.append(buffer, static_cast<std::size_t>(count));
            return;
        }
        if (count == 0) {
            open = false;
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        open = false;
        return;
    }
}

void ReadPipesConcurrently(int stdoutDescriptor, int stderrDescriptor, ProcessResult& result)
{
    bool stdoutOpen = true;
    bool stderrOpen = true;

    while (stdoutOpen || stderrOpen) {
        struct pollfd fds[2]{};
        nfds_t count = 0;
        if (stdoutOpen) {
            fds[count].fd = stdoutDescriptor;
            fds[count].events = POLLIN | POLLHUP | POLLERR;
            ++count;
        }
        if (stderrOpen) {
            fds[count].fd = stderrDescriptor;
            fds[count].events = POLLIN | POLLHUP | POLLERR;
            ++count;
        }

        const auto ready = ::poll(fds, count, -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            stdoutOpen = false;
            stderrOpen = false;
            break;
        }

        for (nfds_t index = 0; index < count; ++index) {
            if (fds[index].revents == 0) {
                continue;
            }
            if (fds[index].fd == stdoutDescriptor) {
                AppendAvailablePipeOutput(stdoutDescriptor, result.stdoutText, stdoutOpen);
            } else if (fds[index].fd == stderrDescriptor) {
                AppendAvailablePipeOutput(stderrDescriptor, result.stderrText, stderrOpen);
            }
        }
    }
}

void ClosePipePair(int pipeDescriptors[2])
{
    if (pipeDescriptors[0] >= 0) {
        ::close(pipeDescriptors[0]);
    }
    if (pipeDescriptors[1] >= 0) {
        ::close(pipeDescriptors[1]);
    }
}

[[nodiscard]] bool CreatePipePair(int pipeDescriptors[2], std::vector<Diagnostic>& diagnostics)
{
    pipeDescriptors[0] = -1;
    pipeDescriptors[1] = -1;
    if (::pipe(pipeDescriptors) != 0) {
        diagnostics.push_back({DiagnosticSeverity::Error, "failed to create process pipes", std::nullopt});
        return false;
    }
    return true;
}

[[nodiscard]] ProcessResult RunProcess(const CommandLine& command, std::vector<Diagnostic>& diagnostics)
{
    ProcessResult result;

    int stdoutPipe[2];
    int stderrPipe[2];
    if (!CreatePipePair(stdoutPipe, diagnostics)) {
        result.exitCode = 1;
        return result;
    }
    if (!CreatePipePair(stderrPipe, diagnostics)) {
        result.exitCode = 1;
        ClosePipePair(stdoutPipe);
        return result;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        diagnostics.push_back({DiagnosticSeverity::Error, "failed to fork process", std::nullopt});
        result.exitCode = 1;
        ClosePipePair(stdoutPipe);
        ClosePipePair(stderrPipe);
        return result;
    }

    if (pid == 0) {
        ::close(stdoutPipe[0]);
        ::close(stderrPipe[0]);

        ::dup2(stdoutPipe[1], STDOUT_FILENO);
        ::dup2(stderrPipe[1], STDERR_FILENO);

        ::close(stdoutPipe[1]);
        ::close(stderrPipe[1]);

        const auto workingDirectory = command.workingDirectory.value_or(std::filesystem::current_path());
        if (::chdir(workingDirectory.c_str()) != 0) {
            const auto message = std::string("failed to chdir: ") + std::strerror(errno) + "\n";
            ::write(STDERR_FILENO, message.data(), message.size());
            ::_exit(127);
        }

        std::vector<char*> argv;
        argv.reserve(command.args.size() + 2);
        argv.push_back(const_cast<char*>(command.program.c_str()));
        for (const auto& arg : command.args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        ::execvp(command.program.c_str(), argv.data());

        const auto message = std::string("failed to exec '") + command.program + "': " + std::strerror(errno) + "\n";
        ::write(STDERR_FILENO, message.data(), message.size());
        ::_exit(127);
    }

    ::close(stdoutPipe[1]);
    ::close(stderrPipe[1]);
    ReadPipesConcurrently(stdoutPipe[0], stderrPipe[0], result);
    ::close(stdoutPipe[0]);
    ::close(stderrPipe[0]);

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        diagnostics.push_back({DiagnosticSeverity::Error, "failed to wait for child process", std::nullopt});
        result.exitCode = 1;
        return result;
    }

    if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exitCode = 128 + WTERMSIG(status);
    } else {
        result.exitCode = 1;
    }
    return result;
}
#else
[[nodiscard]] std::wstring ToWide(const std::string& value)
{
    if (value.empty()) {
        return {};
    }
    const auto size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), size);
    if (!result.empty() && result.back() == L'\0') {
        result.pop_back();
    }
    return result;
}

[[nodiscard]] std::wstring QuoteWindowsArgument(const std::string& value)
{
    if (value.find_first_of(" \t\"") == std::string::npos) {
        return ToWide(value);
    }
    std::wstring output = L"\"";
    unsigned backslashes = 0;
    for (const wchar_t ch : ToWide(value)) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            output.append(backslashes * 2 + 1, L'\\');
            output.push_back(L'"');
            backslashes = 0;
            continue;
        }
        output.append(backslashes, L'\\');
        backslashes = 0;
        output.push_back(ch);
    }
    output.append(backslashes * 2, L'\\');
    output.push_back(L'"');
    return output;
}

[[nodiscard]] std::string ReadHandle(HANDLE handle)
{
    std::string output;
    char buffer[4096];
    DWORD bytesRead = 0;
    while (ReadFile(handle, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead != 0) {
        output.append(buffer, buffer + bytesRead);
    }
    return output;
}

void CloseHandleIfValid(HANDLE handle)
{
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
    }
}

[[nodiscard]] ProcessResult RunProcess(const CommandLine& command, std::vector<Diagnostic>& diagnostics)
{
    ProcessResult result;

    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE stdoutRead = nullptr;
    HANDLE stdoutWrite = nullptr;
    HANDLE stderrRead = nullptr;
    HANDLE stderrWrite = nullptr;
    if (!CreatePipe(&stdoutRead, &stdoutWrite, &attributes, 0) ||
        !CreatePipe(&stderrRead, &stderrWrite, &attributes, 0)) {
        diagnostics.push_back({DiagnosticSeverity::Error, "failed to create process pipes", std::nullopt});
        result.exitCode = 1;
        CloseHandleIfValid(stdoutRead);
        CloseHandleIfValid(stdoutWrite);
        CloseHandleIfValid(stderrRead);
        CloseHandleIfValid(stderrWrite);
        return result;
    }

    SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = stdoutWrite;
    startup.hStdError = stderrWrite;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    std::wstring commandLine = QuoteWindowsArgument(command.program);
    for (const auto& arg : command.args) {
        commandLine.push_back(L' ');
        commandLine += QuoteWindowsArgument(arg);
    }

    PROCESS_INFORMATION process{};
    auto workingDirectory = ToWide(command.workingDirectory.value_or(std::filesystem::current_path()).string());

    if (!CreateProcessW(
            nullptr,
            commandLine.data(),
            nullptr,
            nullptr,
            TRUE,
            0,
            nullptr,
            workingDirectory.c_str(),
            &startup,
            &process)) {
        diagnostics.push_back({DiagnosticSeverity::Error, "failed to create process: " + command.program, std::nullopt});
        result.exitCode = 1;
        CloseHandle(stdoutRead);
        CloseHandle(stdoutWrite);
        CloseHandle(stderrRead);
        CloseHandle(stderrWrite);
        return result;
    }

    CloseHandle(stdoutWrite);
    CloseHandle(stderrWrite);

    std::thread stdoutThread([&result, stdoutRead]() {
        result.stdoutText = ReadHandle(stdoutRead);
    });
    std::thread stderrThread([&result, stderrRead]() {
        result.stderrText = ReadHandle(stderrRead);
    });

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(process.hProcess, &exitCode);
    result.exitCode = static_cast<int>(exitCode);
    stdoutThread.join();
    stderrThread.join();

    CloseHandle(stdoutRead);
    CloseHandle(stderrRead);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return result;
}
#endif

} // namespace

bool ExecuteBuildResult::ok() const
{
    return !HasErrors(diagnostics) && summary.failed == 0;
}

std::string ToString(ActionStatus status)
{
    switch (status) {
    case ActionStatus::Skipped:
        return "skipped";
    case ActionStatus::Executed:
        return "executed";
    case ActionStatus::Failed:
        return "failed";
    }
    return "unknown";
}

[[nodiscard]] bool ValidateActionOrdering(const BuildPlan& plan, std::vector<Diagnostic>& diagnostics)
{
    std::map<std::string, std::size_t> indexById;
    for (std::size_t index = 0; index < plan.actions.size(); ++index) {
        indexById.emplace(plan.actions[index].id, index);
    }

    for (std::size_t index = 0; index < plan.actions.size(); ++index) {
        for (const auto& dependency : plan.actions[index].dependencies) {
            const auto it = indexById.find(dependency);
            if (it == indexById.end()) {
                diagnostics.push_back({DiagnosticSeverity::Error, "action references unknown dependency: " + dependency, std::nullopt});
                return false;
            }
            if (it->second >= index) {
                diagnostics.push_back({DiagnosticSeverity::Error, "action dependency is not ordered before dependent action: " + dependency, std::nullopt});
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] ActionExecution ExecuteAction(
    const BuildPlan& plan,
    const ActionNode& action,
    bool dryRun,
    std::vector<Diagnostic>& diagnostics)
{
    ActionExecution execution;
    execution.actionId = action.id;

    const auto dirty = CheckDirty(plan, action);
    diagnostics.insert(diagnostics.end(), dirty.diagnostics.begin(), dirty.diagnostics.end());
    if (HasErrors(diagnostics)) {
        execution.status = ActionStatus::Failed;
        execution.exitCode = 1;
        execution.reason = dirty.reason;
        return execution;
    }
    if (!dirty.dirty) {
        execution.status = ActionStatus::Skipped;
        execution.reason = dirty.reason;
        return execution;
    }

    if (dryRun) {
        execution.status = ActionStatus::Executed;
        execution.reason = dirty.reason;
        return execution;
    }

    for (const auto& output : action.outputs) {
        EnsureParentDirectory(output.path.absolute, diagnostics);
    }
    if (action.depfile.has_value()) {
        EnsureParentDirectory(action.depfile->absolute, diagnostics);
    }
    EnsureParentDirectory(action.stateFile.absolute, diagnostics);
    if (HasErrors(diagnostics)) {
        execution.status = ActionStatus::Failed;
        execution.exitCode = 1;
        execution.reason = "failed to prepare output directories";
        return execution;
    }

    const auto process = RunProcess(action.command, diagnostics);
    execution.stdoutText = process.stdoutText;
    execution.stderrText = process.stderrText;
    execution.exitCode = process.exitCode;

    if (process.exitCode != 0) {
        execution.status = ActionStatus::Failed;
        execution.reason = dirty.reason;
        diagnostics.push_back({
            DiagnosticSeverity::Error,
            "action failed: " + action.label + " (" + CommandString(action.command) + ")",
            std::nullopt,
        });
        return execution;
    }

    const auto preparedState = PrepareActionState(plan, action);
    diagnostics.insert(diagnostics.end(), preparedState.diagnostics.begin(), preparedState.diagnostics.end());
    if (!preparedState.ok || HasErrors(diagnostics)) {
        execution.status = ActionStatus::Failed;
        execution.reason = "failed to capture action state";
        execution.exitCode = 1;
        return execution;
    }

    WriteActionState(action, preparedState.state, diagnostics);
    if (HasErrors(diagnostics)) {
        execution.status = ActionStatus::Failed;
        execution.reason = "failed to write action state";
        execution.exitCode = 1;
        return execution;
    }
    execution.status = ActionStatus::Executed;
    execution.reason = dirty.reason;
    return execution;
}

void CountExecution(BuildSummary& summary, const ActionExecution& execution)
{
    switch (execution.status) {
    case ActionStatus::Skipped:
        summary.skipped += 1;
        break;
    case ActionStatus::Executed:
        summary.executed += 1;
        break;
    case ActionStatus::Failed:
        summary.failed += 1;
        break;
    }
}

[[nodiscard]] ExecuteBuildResult ExecuteBuildSequential(const ExecuteBuildRequest& request)
{
    ExecuteBuildResult result;
    if (!ValidateActionOrdering(request.plan, result.diagnostics)) {
        return result;
    }

    for (const auto& action : request.plan.actions) {
        std::vector<Diagnostic> diagnostics;
        auto execution = ExecuteAction(request.plan, action, request.dryRun, diagnostics);
        result.diagnostics.insert(result.diagnostics.end(), diagnostics.begin(), diagnostics.end());
        CountExecution(result.summary, execution);
        result.summary.actions.push_back(std::move(execution));
        if (result.summary.failed != 0) {
            return result;
        }
    }

    return result;
}

[[nodiscard]] ExecuteBuildResult ExecuteBuildParallel(const ExecuteBuildRequest& request)
{
    ExecuteBuildResult result;
    if (!ValidateActionOrdering(request.plan, result.diagnostics)) {
        return result;
    }

    const std::size_t actionCount = request.plan.actions.size();
    std::map<std::string, std::size_t> indexById;
    std::vector<std::vector<std::size_t>> dependents(actionCount);
    std::vector<std::size_t> remaining(actionCount, 0);
    for (std::size_t index = 0; index < actionCount; ++index) {
        indexById.emplace(request.plan.actions[index].id, index);
    }
    for (std::size_t index = 0; index < actionCount; ++index) {
        remaining[index] = request.plan.actions[index].dependencies.size();
        for (const auto& dependency : request.plan.actions[index].dependencies) {
            dependents[indexById[dependency]].push_back(index);
        }
    }

    std::queue<std::size_t> ready;
    for (std::size_t index = 0; index < actionCount; ++index) {
        if (remaining[index] == 0) {
            ready.push(index);
        }
    }

    std::vector<bool> completed(actionCount, false);
    std::vector<ActionExecution> executions(actionCount);
    std::size_t finished = 0;
    bool failed = false;
    std::mutex mutex;

    const std::size_t workerCount = std::max<std::size_t>(1, request.jobs);
    auto worker = [&]() {
        while (true) {
            std::size_t index = 0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (failed || ready.empty()) {
                    return;
                }
                index = ready.front();
                ready.pop();
            }

            std::vector<Diagnostic> diagnostics;
            auto execution = ExecuteAction(request.plan, request.plan.actions[index], request.dryRun, diagnostics);

            std::lock_guard<std::mutex> lock(mutex);
            result.diagnostics.insert(result.diagnostics.end(), diagnostics.begin(), diagnostics.end());
            executions[index] = std::move(execution);
            completed[index] = true;
            finished += 1;
            if (executions[index].status == ActionStatus::Failed) {
                failed = true;
                continue;
            }
            for (const std::size_t dependent : dependents[index]) {
                remaining[dependent] -= 1;
                if (remaining[dependent] == 0) {
                    ready.push(dependent);
                }
            }
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (std::size_t index = 0; index < workerCount; ++index) {
        workers.emplace_back(worker);
    }
    for (auto& thread : workers) {
        thread.join();
    }

    for (std::size_t index = 0; index < actionCount; ++index) {
        if (!completed[index]) {
            continue;
        }
        CountExecution(result.summary, executions[index]);
        result.summary.actions.push_back(std::move(executions[index]));
    }

    return result;
}

ExecuteBuildResult ExecuteBuild(const ExecuteBuildRequest& request)
{
    if (request.jobs <= 1 || request.plan.actions.size() <= 1) {
        return ExecuteBuildSequential(request);
    }
    return ExecuteBuildParallel(request);
}

} // namespace scb
