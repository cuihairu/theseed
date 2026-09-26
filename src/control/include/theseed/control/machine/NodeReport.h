#pragma once

#include "theseed/control/machine/HostProbe.h"
#include "theseed/control/machine/ProcessSupervisor.h"

#include <chrono>
#include <string>
#include <vector>

namespace theseed::control::machine {

// 节点快照：主机摘要 + 进程列表 + 节点级状态位。MachineAgent 的采样产物，
// 快照 RPC 与节点上报共用同一数据形状。
struct NodeSummary {
    HostSummary host;
    std::vector<ProcessSummary> processes;
    bool draining = false;
    bool overloaded = false;
};

// 节点摘要上报帧（设计文档 06-machine-agent-and-host-ops §2.4）：
// MachineAgent 周期采样 → 控制面中心聚合。nodeId 取快照时的 hostname
// （与 OpsInspector 的 role+version 语义分层：nodeId 定位机器，快照看状态）。
struct NodeReport {
    std::string nodeId;
    std::chrono::system_clock::time_point timestamp{};
    NodeSummary summary{};
};

// 上报出口：MachineAgent 只依赖本接口（不认识具体控制面中心），
// 测试注入捕获假件，生产接 OpsControlCenter（跨机转发留给跨 realm 平面）。
class INodeReportSink {
public:
    virtual ~INodeReportSink() = default;  // LCOV_EXCL_LINE C++ ABI：trivial 虚析构是空体，gcc 不为其生成计数指令，D0/D1/D2 三符号变体恒 0（结构不可测）

    virtual void publish(const NodeReport& report) = 0;
};

}  // namespace theseed::control::machine
