#include "scb/core/Toolchain.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <sstream>
#include <system_error>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#define NOMINMAX
#include <windows.h>
#endif

namespace scb {
namespace {

[[nodiscard]] bool HasErrors(const std::vector<Diagnostic>& diagnostics)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::Error;
    });
}

[[nodiscard]] std::string Lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

[[nodiscard]] ToolchainFamily InferFamily(const std::filesystem::path& compilerPath)
{
    const auto filename = Lowercase(compilerPath.filename().string());
    if (filename == "cl.exe" || filename == "cl") {
        return ToolchainFamily::Msvc;
    }
    if (filename == "clang++" || filename == "clang++.exe" || filename == "clang-cl.exe" || filename == "clang-cl") {
        return ToolchainFamily::Clang;
    }
    if (filename == "g++" || filename == "g++.exe") {
        return ToolchainFamily::Gcc;
    }
    return ToolchainFamily::Unknown;
}

[[nodiscard]] std::vector<std::filesystem::path> PathEntries()
{
    std::vector<std::filesystem::path> entries;
    const char* path = std::getenv("PATH");
    if (path == nullptr) {
        return entries;
    }

#ifdef _WIN32
    constexpr char separator = ';';
#else
    constexpr char separator = ':';
#endif

    std::stringstream stream(path);
    std::string entry;
    while (std::getline(stream, entry, separator)) {
        if (!entry.empty()) {
            entries.emplace_back(entry);
        }
    }
    return entries;
}

[[nodiscard]] std::optional<std::filesystem::path> FindExecutable(const std::string& executable)
{
    const std::filesystem::path candidate(executable);
    if (candidate.is_absolute() || candidate.has_parent_path()) {
        if (std::filesystem::exists(candidate)) {
            return std::filesystem::absolute(candidate).lexically_normal();
        }
        return std::nullopt;
    }

    for (const auto& entry : PathEntries()) {
        const auto path = (entry / candidate).lexically_normal();
        if (std::filesystem::exists(path) && !std::filesystem::is_directory(path)) {
            return path;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::filesystem::path> DetectArchiver(ToolchainFamily family, const std::filesystem::path& compilerPath)
{
    if (family == ToolchainFamily::Msvc) {
        const auto sibling = compilerPath.parent_path() / "lib.exe";
        if (std::filesystem::exists(sibling)) {
            return sibling.lexically_normal();
        }
        if (auto libExe = FindExecutable("lib.exe")) {
            return libExe;
        }
        return FindExecutable("lib");
    }

    if (family == ToolchainFamily::Clang) {
        if (auto llvmAr = FindExecutable("llvm-ar")) {
            return llvmAr;
        }
    }
    return FindExecutable("ar");
}

[[nodiscard]] std::optional<std::filesystem::path> DetectLinker(ToolchainFamily family, const std::filesystem::path& compilerPath)
{
    if (family == ToolchainFamily::Msvc) {
        const auto sibling = compilerPath.parent_path() / "link.exe";
        if (std::filesystem::exists(sibling)) {
            return sibling.lexically_normal();
        }
        if (auto linkExe = FindExecutable("link.exe")) {
            return linkExe;
        }
        return FindExecutable("link");
    }
    return compilerPath;
}

#ifndef _WIN32
[[nodiscard]] std::optional<std::string> RunCommandAndCapture(
    const std::filesystem::path& program,
    const std::vector<std::string>& args)
{
    int pipeDescriptors[2];
    if (::pipe(pipeDescriptors) != 0) {
        return std::nullopt;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipeDescriptors[0]);
        ::close(pipeDescriptors[1]);
        return std::nullopt;
    }

    if (pid == 0) {
        ::close(pipeDescriptors[0]);
        ::dup2(pipeDescriptors[1], STDOUT_FILENO);
        ::dup2(pipeDescriptors[1], STDERR_FILENO);
        ::close(pipeDescriptors[1]);

        std::vector<std::string> argvStorage;
        argvStorage.reserve(args.size() + 1);
        argvStorage.push_back(program.string());
        argvStorage.insert(argvStorage.end(), args.begin(), args.end());

        std::vector<char*> argv;
        argv.reserve(argvStorage.size() + 1);
        for (auto& value : argvStorage) {
            argv.push_back(value.data());
        }
        argv.push_back(nullptr);

        ::execv(program.c_str(), argv.data());
        ::_exit(127);
    }

    ::close(pipeDescriptors[1]);

    std::string output;
    char buffer[4096];
    while (true) {
        const auto count = ::read(pipeDescriptors[0], buffer, sizeof(buffer));
        if (count > 0) {
            output.append(buffer, static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) {
            break;
        }
        ::close(pipeDescriptors[0]);
        return std::nullopt;
    }
    ::close(pipeDescriptors[0]);

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || output.empty()) {
        return std::nullopt;
    }
    return output;
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

void CloseHandleIfValid(HANDLE handle)
{
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
    }
}

[[nodiscard]] std::optional<std::string> RunCommandAndCapture(
    const std::filesystem::path& program,
    const std::vector<std::string>& args)
{
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE outputRead = nullptr;
    HANDLE outputWrite = nullptr;
    if (!CreatePipe(&outputRead, &outputWrite, &attributes, 0)) {
        return std::nullopt;
    }
    SetHandleInformation(outputRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = outputWrite;
    startup.hStdError = outputWrite;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    std::wstring commandLine = QuoteWindowsArgument(program.string());
    for (const auto& arg : args) {
        commandLine.push_back(L' ');
        commandLine += QuoteWindowsArgument(arg);
    }

    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            nullptr,
            commandLine.data(),
            nullptr,
            nullptr,
            TRUE,
            0,
            nullptr,
            nullptr,
            &startup,
            &process)) {
        CloseHandleIfValid(outputRead);
        CloseHandleIfValid(outputWrite);
        return std::nullopt;
    }

    CloseHandleIfValid(outputWrite);
    outputWrite = nullptr;

    std::string output;
    char buffer[4096];
    DWORD bytesRead = 0;
    while (ReadFile(outputRead, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead != 0) {
        output.append(buffer, buffer + bytesRead);
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);

    CloseHandleIfValid(outputRead);
    CloseHandleIfValid(process.hProcess);
    CloseHandleIfValid(process.hThread);

    if (exitCode != 0 || output.empty()) {
        return std::nullopt;
    }
    return output;
}
#endif

[[nodiscard]] std::string Trim(std::string value)
{
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    std::size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) {
        ++start;
    }
    return value.substr(start);
}

[[nodiscard]] std::string FirstNonEmptyLine(const std::string& text)
{
    std::stringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        line = Trim(line);
        if (!line.empty()) {
            return line;
        }
    }
    return {};
}

[[nodiscard]] std::string StableHashHex(std::string_view input)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char character : input) {
        hash ^= character;
        hash *= 1099511628211ull;
    }

    std::ostringstream stream;
    stream << std::hex << hash;
    return stream.str();
}

[[nodiscard]] std::string FallbackIdentityPayload(const std::filesystem::path& compilerPath)
{
    std::error_code error;
    const auto writeTime = std::filesystem::last_write_time(compilerPath, error);
    std::ostringstream stream;
    stream << compilerPath.lexically_normal().string();
    if (!error) {
        stream << '\n' << writeTime.time_since_epoch().count();
    }
    return stream.str();
}

void PopulateToolchainIdentity(ToolchainInfo& toolchain)
{
    const std::filesystem::path compilerPath(toolchain.compilerPath);

    std::optional<std::string> probeOutput;
    if (toolchain.family == ToolchainFamily::Msvc) {
        probeOutput = RunCommandAndCapture(compilerPath, {"/Bv"});
        if (!probeOutput.has_value()) {
            probeOutput = RunCommandAndCapture(compilerPath, {"/?"});
        }
    } else {
        probeOutput = RunCommandAndCapture(compilerPath, {"--version"});
    }

    if (probeOutput.has_value()) {
        toolchain.version = FirstNonEmptyLine(*probeOutput);
        toolchain.identity = StableHashHex(compilerPath.lexically_normal().string() + "\n" + *probeOutput);
        return;
    }

    if (toolchain.version.empty()) {
        toolchain.version = compilerPath.filename().string();
    }
    toolchain.identity = StableHashHex(FallbackIdentityPayload(compilerPath));
}

} // namespace

bool DetectToolchainResult::ok() const
{
    return !HasErrors(diagnostics);
}

DetectToolchainResult DetectHostToolchain(const DetectToolchainRequest& request)
{
    DetectToolchainResult result;

    std::optional<std::filesystem::path> compilerPath;
    if (request.compilerOverride.has_value()) {
        std::filesystem::path overridePath(*request.compilerOverride);
        if (overridePath.is_relative() && overridePath.has_parent_path()) {
            overridePath = request.projectRoot / overridePath;
        }
        compilerPath = FindExecutable(overridePath.string());
        if (!compilerPath.has_value()) {
            result.diagnostics.push_back({
                DiagnosticSeverity::Error,
                "configured compiler does not exist: " + overridePath.string(),
                std::nullopt,
            });
            return result;
        }
    } else {
        for (const auto& candidate : {std::string("cl.exe"), std::string("cl"), std::string("g++"), std::string("clang++")}) {
            compilerPath = FindExecutable(candidate);
            if (compilerPath.has_value()) {
                break;
            }
        }
        if (!compilerPath.has_value()) {
            result.diagnostics.push_back({
                DiagnosticSeverity::Error,
                "no supported C++ compiler found in PATH",
                std::nullopt,
            });
            return result;
        }
    }

    result.toolchain.compilerPath = compilerPath->string();
    result.toolchain.family = InferFamily(*compilerPath);
    if (result.toolchain.family == ToolchainFamily::Unknown) {
        result.diagnostics.push_back({
            DiagnosticSeverity::Error,
            "unable to determine compiler family for: " + compilerPath->string(),
            std::nullopt,
        });
        return result;
    }

    if (auto archiver = DetectArchiver(result.toolchain.family, *compilerPath)) {
        result.toolchain.archiverPath = archiver->string();
    }
    if (auto linker = DetectLinker(result.toolchain.family, *compilerPath)) {
        result.toolchain.linkerPath = linker->string();
    }
    PopulateToolchainIdentity(result.toolchain);

    return result;
}

} // namespace scb
