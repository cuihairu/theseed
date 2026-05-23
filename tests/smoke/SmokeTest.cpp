#include "theseed/control/machine/MachineAgent.h"
#include "theseed/control/machine/MachineSnapshotCodec.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

using theseed::control::machine::MachineAgent;
using theseed::control::machine::LocalHostProbe;
using theseed::control::machine::LocalProcessSupervisor;
using theseed::control::machine::formatSnapshotJson;

namespace {

std::uint32_t currentProcessId() {
#ifdef _WIN32
    return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}

const theseed::control::machine::ProcessSummary* findCurrentProcess(
    const std::vector<theseed::control::machine::ProcessSummary>& processes) {
    const auto pid = currentProcessId();
    for (const auto& process : processes) {
        if (process.pid == pid) {
            return &process;
        }
    }

    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    auto fail = [](const char* stage) {
        std::cerr << "smoke_failed_at=" << stage << '\n';
        return EXIT_FAILURE;
    };

    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == "--child-sleep") {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            return EXIT_SUCCESS;
        }

        if (std::string(argv[index]) == "--child-exit") {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            return EXIT_SUCCESS;
        }
    }

    MachineAgent agent(
        std::make_unique<LocalHostProbe>(),
        std::make_unique<LocalProcessSupervisor>());

    const auto summary = agent.snapshot();
    if (summary.host.hostname.empty()) {
        return fail("host");
    }

    if (summary.processes.empty()) {
        return fail("processes_empty");
    }

    const auto* process = findCurrentProcess(summary.processes);
    if (process == nullptr) {
        return fail("current_process_missing");
    }

    if (process->pid == 0) {
        return fail("current_pid_zero");
    }

    if (process->name.empty()) {
        return fail("current_name_empty");
    }

    if (!process->healthy) {
        return fail("current_not_healthy");
    }

    const auto json = formatSnapshotJson(summary);
    if (json.find("\"host\"") == std::string::npos) {
        return fail("json_missing_host");
    }

    if (agent.execute("stop", "99999999")) {
        return fail("invalid_stop_succeeded");
    }

    auto supervisor = std::make_unique<LocalProcessSupervisor>();
    const auto selfPath = std::filesystem::absolute(argv[0]).string();
    const auto selfName = std::filesystem::path(selfPath).filename().string();
    const auto childCommand = "\"" + selfPath + "\" --child-sleep";

    if (!supervisor->start(childCommand)) {
        return fail("start_sleep_child");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto processes = supervisor->listProcesses();
    const auto it = std::find_if(
        processes.begin(),
        processes.end(),
        [&](const auto& candidate) {
            return candidate.pid != currentProcessId() && candidate.name == selfName &&
                   candidate.managed;
        });

    if (it == processes.end()) {
        return fail("managed_child_not_found");
    }

    if (!supervisor->stop(it->pid)) {
        return fail("stop_sleep_child");
    }

    const auto exitCommand = "\"" + selfPath + "\" --child-exit";
    if (!supervisor->start(exitCommand)) {
        return fail("start_exit_child");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    const auto exitedProcesses = supervisor->listProcesses();
    const auto exitedChild = std::find_if(
        exitedProcesses.begin(),
        exitedProcesses.end(),
        [&](const auto& candidate) {
            return candidate.pid != currentProcessId() && candidate.name == selfName &&
                   candidate.managed;
        });

    if (exitedChild != exitedProcesses.end()) {
        return fail("exited_child_still_listed");
    }

    return EXIT_SUCCESS;
}
