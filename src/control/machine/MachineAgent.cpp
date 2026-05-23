#include "theseed/control/machine/MachineAgent.h"

#include <charconv>

namespace theseed::control::machine {

MachineAgent::MachineAgent(std::unique_ptr<IHostProbe> hostProbe,
                           std::unique_ptr<IProcessSupervisor> processSupervisor)
    : hostProbe_(std::move(hostProbe)),
      processSupervisor_(std::move(processSupervisor)) {}

NodeSummary MachineAgent::snapshot() {
    NodeSummary summary;
    summary.host = hostProbe_->sample();
    summary.processes = processSupervisor_->listProcesses();
    return summary;
}

bool MachineAgent::execute(const std::string& command, const std::string& args) {
    if (command == "start") {
        return processSupervisor_->start(args);
    }

    if (command == "stop") {
        std::uint32_t pid = 0;
        const auto* begin = args.data();
        const auto* end = begin + args.size();
        const auto [ptr, ec] = std::from_chars(begin, end, pid);
        if (ec != std::errc{} || ptr != end) {
            return false;
        }
        return processSupervisor_->stop(pid);
    }

    if (command == "restart") {
        std::uint32_t pid = 0;
        const auto* begin = args.data();
        const auto* end = begin + args.size();
        const auto [ptr, ec] = std::from_chars(begin, end, pid);
        if (ec != std::errc{} || ptr != end) {
            return false;
        }
        return processSupervisor_->restart(pid);
    }

    return false;
}

}  // namespace theseed::control::machine
