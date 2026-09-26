#pragma once

#include "theseed/runtime/RuntimeTypes.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace theseed::control::machine {

// 剖面元数据（04-ops-control-plane §7 中心侧维度视图的最小形状）：agent
// 经 report() 通道上报、中心聚合供"按 entity / entity type / process
// 查询当前剖面"。
//
// 维度纪律（诚实缺省，不编造数据）：机器 agent 的 TickProfiler 是进程级
// tick 粒度采样——entityId / entityType 无生产者，agent 侧恒空上报；
// 中心侧按这两维查询如实落空。process 维 ≙ nodeId（06 的 nodeId 口径 =
// 快照 hostname，定位"哪台机的 agent 进程"），不冗余进本结构——归属由
// 上报帧（NodeReport.nodeId / NodeProfileArtifact.nodeId）承载。
struct ProfileMeta final {
    std::uint64_t handle = 0;
    std::uint64_t tickCount = 0;
    double windowMs = 0.0;
    std::string entityId;    // 无生产者，恒空（诚实缺省）
    std::string entityType;  // 无生产者，恒空（诚实缺省）
};

// 剖面元数据来源（能力而非义务接缝）：MachineAgent 组装 NodeReport 时
// 向其拉取本机当前剖面清单；nullptr = 无剖面（报告照发，profiles 留空）。
// MachineDaemon 持有采样出口，是自然实现方。
class IProfileMetaSource {
public:
    virtual ~IProfileMetaSource() = default;  // LCOV_EXCL_LINE C++ ABI：trivial 虚析构是空体，gcc 不为其生成计数指令，D0/D1/D2 三符号变体恒 0（结构不可测）

    virtual std::vector<ProfileMeta> profileMetas() const = 0;
};

// 诊断采样入口策略（04 §7：触发/下载鉴权归 Ops Control Plane）。从
// MachineDaemon 嵌套定义上提为命名空间级：中心侧查询/下载沿用同一
// 读位（canAccess），授权口径不复制不走样；触发位（canTrigger）只在
// agent 侧生效——本批无中心侧写命令（只读优先）。
//
// - canTrigger：允许触发采样窗口（写侧，消耗主机性能预算）；
// - canAccess：允许查询产物清单与下载（读侧，明细外泄面）。
// 两个集合都为空集语义 = 一律拒绝（安全缺省：未显式授权即不可用）。
struct DiagnosticsPolicy final {
    std::vector<runtime::ComponentId> canTrigger;
    std::vector<runtime::ComponentId> canAccess;
};

// 中心侧维度查询（只读）：字段为空 = 该维度不过滤。process 维由
// nodeId 承载（见 ProfileMeta 的维度纪律）。
struct ProfileQuery final {
    std::string nodeId;
    std::string entityId;
    std::string entityType;
};

// 查询结果行：节点归属 + 剖面元数据（归属即 process 维的取值）。
struct NodeProfileEntry final {
    std::string nodeId;
    ProfileMeta meta{};
};

// 产物回传帧：采样窗口固化后由 agent 推送中心存储的只读字节 + 元数据。
// 句柄语义沿用 agent 侧（单调递增、按 (nodeId, handle) 唯一）。
struct NodeProfileArtifact final {
    std::string nodeId;
    ProfileMeta meta{};
    std::string payload;  // 产物 JSON 快照字节（agent 侧下载的同源内容）
};

// 产物回传出口（agent → 中心推送方向的接缝，与 INodeAuditSink 同族）：
// daemon 在 tick 里发现新固化产物即推送（拉取需要中心→agent 反向连接，
// 当前拓扑只有 agent→center 单向通道，见 todo.md 选型记录）。
class INodeArtifactSink {
public:
    virtual ~INodeArtifactSink() = default;  // LCOV_EXCL_LINE C++ ABI：trivial 虚析构是空体，gcc 不为其生成计数指令，D0/D1/D2 三符号变体恒 0（结构不可测）

    virtual void publish(const NodeProfileArtifact& artifact) = 0;
};

}  // namespace theseed::control::machine
