#include "theseed/control/machine/ProcessSupervisor.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace theseed::control::machine {

namespace {

std::string stripQuotes(const std::string& text) {
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        return text.substr(1, text.size() - 2);
    }

    return text;
}

std::string basenameOfCommandLine(const std::string& commandLine) {
    const std::string stripped = stripQuotes(commandLine);
    if (stripped.empty()) {
        return "unknown";
    }

    return std::filesystem::path(stripped).filename().string();
}

std::vector<std::string> tokenizeCommandLine(const std::string& commandLine) {
    std::vector<std::string> tokens;
    std::string current;
    bool inQuotes = false;

    for (const char ch : commandLine) {
        if (ch == '"') {
            inQuotes = !inQuotes;
            continue;
        }

        if (std::isspace(static_cast<unsigned char>(ch)) != 0 && !inQuotes) {
            if (!current.empty()) {
                tokens.push_back(std::move(current));
                current.clear();
            }
            continue;
        }

        current.push_back(ch);
    }

    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }

    return tokens;
}

void upsertProcess(std::vector<ProcessSummary>& processes, ProcessSummary summary) {
    for (auto& process : processes) {
        if (process.pid == summary.pid) {
            process = std::move(summary);
            return;
        }
    }

    processes.push_back(std::move(summary));
}

ProcessSummary queryCurrentProcess() {
    ProcessSummary summary;
    summary.healthy = true;

#ifdef _WIN32
    summary.pid = static_cast<std::uint32_t>(GetCurrentProcessId());

    char buffer[MAX_PATH] = {};
    const auto length = GetModuleFileNameA(nullptr, buffer, static_cast<DWORD>(sizeof(buffer)));
    if (length != 0) {
        const auto path = std::filesystem::path(std::string(buffer, length));
        summary.name = path.filename().string();
    }
#else
    summary.pid = static_cast<std::uint32_t>(getpid());

#if defined(__linux__)
    std::ifstream input("/proc/self/comm");
    std::getline(input, summary.name);
#endif
#endif

    if (summary.name.empty()) {
        summary.name = "theseed_machine";
    }

    return summary;
}

std::vector<ProcessSummary> enumerateWindowsProcesses() {
    std::vector<ProcessSummary> processes;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return processes;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry) != 0) {
        do {
            ProcessSummary summary;
            summary.pid = entry.th32ProcessID;
            summary.healthy = true;
            summary.managed = false;

            const int required = WideCharToMultiByte(
                CP_UTF8,
                0,
                entry.szExeFile,
                -1,
                nullptr,
                0,
                nullptr,
                nullptr);

            if (required > 1) {
                std::string name(static_cast<std::size_t>(required - 1), '\0');
                WideCharToMultiByte(
                    CP_UTF8,
                    0,
                    entry.szExeFile,
                    -1,
                    name.data(),
                    required,
                    nullptr,
                    nullptr);
                summary.name = std::move(name);
            }

            if (summary.name.empty()) {
                summary.name = "unknown";
            }

            processes.push_back(std::move(summary));
        } while (Process32NextW(snapshot, &entry) != 0);
    }

    CloseHandle(snapshot);
    return processes;
}

#if defined(__linux__)
bool isNumericDirectory(const std::filesystem::directory_entry& entry) {
    if (!entry.is_directory()) {
        return false;
    }

    const auto name = entry.path().filename().string();
    return !name.empty() &&
           std::all_of(name.begin(), name.end(), [](unsigned char ch) {
               return std::isdigit(ch) != 0;
           });
}

std::vector<ProcessSummary> enumerateLinuxProcesses() {
    std::vector<ProcessSummary> processes;
    std::error_code error;

    for (const auto& entry : std::filesystem::directory_iterator("/proc", error)) {
        if (error || !isNumericDirectory(entry)) {
            continue;
        }

        ProcessSummary summary;
        summary.pid = static_cast<std::uint32_t>(
            std::strtoul(entry.path().filename().string().c_str(), nullptr, 10));
        summary.healthy = true;
        summary.managed = false;

        std::ifstream commFile(entry.path() / "comm");
        std::getline(commFile, summary.name);
        if (summary.name.empty()) {
            summary.name = entry.path().filename().string();
        }

        processes.push_back(std::move(summary));
    }

    return processes;
}
#endif

#if defined(__APPLE__)
std::vector<ProcessSummary> enumerateMacProcesses() {
    std::vector<ProcessSummary> processes;
    FILE* pipe = popen("ps -axo pid=,comm=", "r");
    if (pipe == nullptr) {
        return processes;
    }

    char buffer[1024] = {};
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        std::string line(buffer);
        const auto first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            continue;
        }

        const auto separator = line.find_first_of(" \t", first);
        if (separator == std::string::npos) {
            continue;
        }

        ProcessSummary summary;
        summary.pid = static_cast<std::uint32_t>(
            std::strtoul(line.substr(first, separator - first).c_str(), nullptr, 10));
        summary.healthy = true;
        summary.managed = false;

        summary.name = line.substr(line.find_first_not_of(" \t", separator));
        summary.name.erase(summary.name.find_last_not_of("\r\n") + 1);

        if (!summary.name.empty()) {
            processes.push_back(std::move(summary));
        }
    }

    pclose(pipe);
    return processes;
}
#endif

}  // namespace

struct LocalProcessSupervisor::ChildProcess {
    std::string commandLine;
    std::string name;

#ifdef _WIN32
    PROCESS_INFORMATION processInfo{};
#else
    pid_t pid = -1;
#endif
};

#ifdef _WIN32
bool hasProcessExited(HANDLE processHandle) {
    DWORD exitCode = STILL_ACTIVE;
    if (GetExitCodeProcess(processHandle, &exitCode) == 0) {
        return false;
    }

    return exitCode != STILL_ACTIVE;
}
#else
bool hasProcessExited(pid_t pid) {
    int status = 0;
    const pid_t waited = waitpid(pid, &status, WNOHANG);
    if (waited == 0) {
        return false;
    }

    return waited == pid || (waited == -1 && errno == ECHILD);
}
#endif

LocalProcessSupervisor::LocalProcessSupervisor() = default;

LocalProcessSupervisor::~LocalProcessSupervisor() {
    std::vector<std::uint32_t> managedPids;
    {
        std::lock_guard lock(mutex_);
        managedPids.reserve(managedProcesses_.size());
        for (const auto& [pid, process] : managedProcesses_) {
            static_cast<void>(process);
            managedPids.push_back(pid);
        }
    }

    for (const auto pid : managedPids) {
        (void)terminateManagedProcess(pid);
    }
}

std::vector<std::string> LocalProcessSupervisor::splitCommandLine(const std::string& commandLine) {
    return tokenizeCommandLine(commandLine);
}

std::string LocalProcessSupervisor::basenameOf(const std::string& commandLine) {
    return basenameOfCommandLine(commandLine);
}

std::vector<ProcessSummary> LocalProcessSupervisor::listProcesses() const {
    reapManagedProcesses();

    std::vector<ProcessSummary> processes;

#if defined(_WIN32)
    processes = enumerateWindowsProcesses();
#elif defined(__linux__)
    processes = enumerateLinuxProcesses();
#elif defined(__APPLE__)
    processes = enumerateMacProcesses();
#endif

    if (processes.empty()) {
        processes.push_back(queryCurrentProcess());
    }

    {
        std::lock_guard lock(mutex_);
        for (const auto& [pid, child] : managedProcesses_) {
        ProcessSummary summary;
        summary.pid = pid;
        summary.name = child->name;
        summary.healthy = true;
        summary.managed = true;
        upsertProcess(processes, std::move(summary));
    }
}

    return processes;
}

bool LocalProcessSupervisor::start(const std::string& target) {
    if (target.empty()) {
        return false;
    }

    const auto tokens = splitCommandLine(target);
    if (tokens.empty()) {
        return false;
    }

#ifdef _WIN32
    std::vector<char> commandLine(target.begin(), target.end());
    commandLine.push_back('\0');

    STARTUPINFOA startupInfo{};
    startupInfo.cb = sizeof(startupInfo);

    PROCESS_INFORMATION processInfo{};
    if (CreateProcessA(
            nullptr,
            commandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startupInfo,
            &processInfo) == 0) {
        return false;
    }

    auto child = std::make_unique<ChildProcess>();
    child->commandLine = target;
    child->name = basenameOf(tokens.front());
    child->processInfo = processInfo;

    std::lock_guard lock(mutex_);
    managedProcesses_[processInfo.dwProcessId] = std::move(child);
    return true;
#else
    const pid_t pid = fork();
    if (pid < 0) {
        return false;
    }

    if (pid == 0) {
        std::vector<char*> argv;
        argv.reserve(tokens.size() + 1);
        for (auto& token : tokens) {
            argv.push_back(token.data());
        }
        argv.push_back(nullptr);

        execvp(argv[0], argv.data());
        _exit(127);
    }

    auto child = std::make_unique<ChildProcess>();
    child->commandLine = target;
    child->name = basenameOf(tokens.front());
    child->pid = pid;

    std::lock_guard lock(mutex_);
    managedProcesses_[static_cast<std::uint32_t>(pid)] = std::move(child);
    return true;
#endif
}

bool LocalProcessSupervisor::stop(std::uint32_t pid) {
    return terminateManagedProcess(pid);
}

bool LocalProcessSupervisor::restart(std::uint32_t pid) {
    reapManagedProcesses();

    std::string commandLine;
    {
        std::lock_guard lock(mutex_);
        const auto iter = managedProcesses_.find(pid);
        if (iter == managedProcesses_.end()) {
            return false;
        }

        commandLine = iter->second->commandLine;
    }

    if (!terminateManagedProcess(pid)) {
        return false;
    }

    return start(commandLine);
}

void LocalProcessSupervisor::reapManagedProcesses() const {
    std::lock_guard lock(mutex_);

    for (auto iter = managedProcesses_.begin(); iter != managedProcesses_.end();) {
#ifdef _WIN32
        if (hasProcessExited(iter->second->processInfo.hProcess)) {
            CloseHandle(iter->second->processInfo.hThread);
            CloseHandle(iter->second->processInfo.hProcess);
            iter = managedProcesses_.erase(iter);
            continue;
        }
#else
        if (hasProcessExited(iter->second->pid)) {
            iter = managedProcesses_.erase(iter);
            continue;
        }
#endif

        ++iter;
    }
}

bool LocalProcessSupervisor::terminateManagedProcess(std::uint32_t pid) {
    std::unique_ptr<ChildProcess> child;
    {
        std::lock_guard lock(mutex_);
        const auto iter = managedProcesses_.find(pid);
        if (iter == managedProcesses_.end()) {
            return false;
        }

        child = std::move(iter->second);
        managedProcesses_.erase(iter);
    }

#ifdef _WIN32
    const bool terminated = TerminateProcess(child->processInfo.hProcess, 0) != 0;
    WaitForSingleObject(child->processInfo.hProcess, 1000);
    CloseHandle(child->processInfo.hThread);
    CloseHandle(child->processInfo.hProcess);
    return terminated;
#else
    if (kill(child->pid, SIGTERM) != 0 && errno != ESRCH) {
        return false;
    }

    int status = 0;
    const pid_t waited = waitpid(child->pid, &status, 0);
    return waited == child->pid || (waited == -1 && errno == ECHILD);
#endif
}

}  // namespace theseed::control::machine
