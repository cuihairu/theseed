#include "theseed/control/machine/MachineAgent.h"
#include "theseed/control/machine/MachineSnapshotCodec.h"

#include <iostream>
#include <memory>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#define THESEED_ARGC __argc
#define THESEED_ARGV __argv
#else
#define THESEED_ARGC argc
#define THESEED_ARGV argv
#endif

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
using theseed::control::machine::formatSnapshotText;

int main(int argc, char** argv) {
    static_cast<void>(argc);
    static_cast<void>(argv);

    bool useJson = false;
    std::string command;
    std::string argument;

    for (int index = 1; index < THESEED_ARGC; ++index) {
        const std::string current = THESEED_ARGV[index];
        if (current == "--json") {
            useJson = true;
            continue;
        }

        if (command.empty()) {
            command = current;
        } else if (argument.empty()) {
            argument = current;
        }
    }

    MachineAgent agent(
        std::make_unique<LocalHostProbe>(),
        std::make_unique<LocalProcessSupervisor>());

    if (!command.empty()) {
        const bool ok = agent.execute(command, argument);
        std::cout << "command=" << command << "\n";
        std::cout << "command_ok=" << (ok ? "true" : "false") << "\n";
    }

    const auto summary = agent.snapshot();

    std::cout << "theseed_machine bootstrap\n";
    if (useJson) {
        std::cout << formatSnapshotJson(summary) << "\n";
    } else {
        std::cout << formatSnapshotText(summary);
    }

    return 0;
}
