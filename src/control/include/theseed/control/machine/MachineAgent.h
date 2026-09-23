#pragma once

#include "theseed/control/machine/HostProbe.h"
#include "theseed/control/machine/ProcessSupervisor.h"

#include <memory>
#include <string>

namespace theseed::control::machine {

struct NodeSummary {
    HostSummary host;
    std::vector<ProcessSummary> processes;
    bool draining = false;
    bool overloaded = false;
};

class IMachineAgent {
public:
    virtual ~IMachineAgent() = default;  // LCOV_EXCL_LINE C++ ABI：trivial 虚析构是空体，gcc 不为其生成计数指令，D0/D1/D2 三符号变体恒 0（结构不可测）

    virtual NodeSummary snapshot() = 0;
    virtual bool execute(const std::string& command, const std::string& args) = 0;
};

class MachineAgent final : public IMachineAgent {
public:
    MachineAgent(std::unique_ptr<IHostProbe> hostProbe,
                 std::unique_ptr<IProcessSupervisor> processSupervisor);

    NodeSummary snapshot() override;
    bool execute(const std::string& command, const std::string& args) override;

private:
    std::unique_ptr<IHostProbe> hostProbe_;
    std::unique_ptr<IProcessSupervisor> processSupervisor_;
};

}  // namespace theseed::control::machine
