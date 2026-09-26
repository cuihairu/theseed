#pragma once

#include "theseed/control/machine/HostProbe.h"
#include "theseed/control/machine/NodeReport.h"
#include "theseed/control/machine/ProcessSupervisor.h"

#include <memory>
#include <string>

namespace theseed::control::machine {

class IMachineAgent {
public:
    virtual ~IMachineAgent() = default;  // LCOV_EXCL_LINE C++ ABI：trivial 虚析构是空体，gcc 不为其生成计数指令，D0/D1/D2 三符号变体恒 0（结构不可测）

    virtual NodeSummary snapshot() = 0;
    virtual bool execute(const std::string& command, const std::string& args) = 0;

    // 节点摘要上报（设计文档 06 §2.4/§4.3）：采一次快照推给上报出口。
    // 无出口（未注册）时为空操作——上报是能力而非义务。
    virtual void report() = 0;
};

class MachineAgent final : public IMachineAgent {
public:
    MachineAgent(std::unique_ptr<IHostProbe> hostProbe,
                 std::unique_ptr<IProcessSupervisor> processSupervisor);
    // 注册上报出口的便捷构造；sink 不持有（调用方保证生命周期覆盖 agent）。
    MachineAgent(std::unique_ptr<IHostProbe> hostProbe,
                 std::unique_ptr<IProcessSupervisor> processSupervisor,
                 INodeReportSink* reportSink);

    NodeSummary snapshot() override;
    bool execute(const std::string& command, const std::string& args) override;
    void report() override;

    // 运行期换绑上报出口（nullptr = 关闭上报）。
    void setReportSink(INodeReportSink* reportSink);

private:
    std::unique_ptr<IHostProbe> hostProbe_;
    std::unique_ptr<IProcessSupervisor> processSupervisor_;
    INodeReportSink* reportSink_ = nullptr;
};

}  // namespace theseed::control::machine
