#pragma once

#include "theseed/control/machine/HostProbe.h"
#include "theseed/control/machine/NodeReport.h"
#include "theseed/control/machine/ProcessSupervisor.h"

#include <memory>
#include <string>
#include <vector>

namespace theseed::control::machine {

class IMachineAgent {
public:
    virtual ~IMachineAgent() = default;  // LCOV_EXCL_LINE C++ ABI：trivial 虚析构是空体，gcc 不为其生成计数指令，D0/D1/D2 三符号变体恒 0（结构不可测）

    virtual NodeSummary snapshot() = 0;
    virtual bool execute(const std::string& command, const std::string& args) = 0;

    // 主机级进程枚举（06 §7 MVP "process list / state / pid"）：全主机
    // 进程表（含 managed 标记），治理侧据此过滤非受控目标并做保护判定。
    // 与 snapshot 的差别：不采主机资源摘要（枚举只读、高频调用无谓开销）。
    virtual std::vector<ProcessSummary> enumerateHostProcesses() = 0;

    // 非受控进程处置转发：受管进程拒绝（单一控制路径纪律，见
    // IProcessSupervisor::terminateUnmanaged）。策略判定在 daemon 侧，
    // 这里只做能力转发。
    virtual bool terminateHostProcess(std::uint32_t pid) = 0;

    // 节点摘要上报（设计文档 06 §2.4/§4.3）：采一次快照推给上报出口。
    // 无出口（未注册）时为空操作——上报是能力而非义务。
    virtual void report() = 0;

    // §6.1 set draining：翻转节点排水位（不再承接新工作，存量照常）。
    // agent 是节点状态的持有方——置位后 snapshot()（快照 RPC 与节点
    // 上报共用形状）即带该位，经既有 report() 通道汇聚到中心。
    virtual void setDraining(bool draining) = 0;

    // 绑定剖面元数据来源（04 §7 中心侧维度视图的汇聚通道）：report()
    // 组装 NodeReport 时拉取本机当前剖面清单；nullptr = 无剖面（报告照
    // 发，profiles 留空）。不持有（调用方保证生命周期覆盖 agent）。
    virtual void setProfileMetaSource(
        IProfileMetaSource* profileMetaSource) = 0;
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
    std::vector<ProcessSummary> enumerateHostProcesses() override;
    bool terminateHostProcess(std::uint32_t pid) override;
    void report() override;
    void setDraining(bool draining) override;
    void setProfileMetaSource(
        IProfileMetaSource* profileMetaSource) override;

    // 运行期换绑上报出口（nullptr = 关闭上报）。
    void setReportSink(INodeReportSink* reportSink);

private:
    std::unique_ptr<IHostProbe> hostProbe_;
    std::unique_ptr<IProcessSupervisor> processSupervisor_;
    INodeReportSink* reportSink_ = nullptr;
    IProfileMetaSource* profileMetaSource_ = nullptr;
    // §6.1 排水位（agent 持有的节点状态：snapshot()/report() 即透出）。
    bool draining_ = false;
};

}  // namespace theseed::control::machine
