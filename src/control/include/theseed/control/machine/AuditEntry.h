#pragma once

#include "theseed/runtime/RuntimeTypes.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace theseed::control::machine {

// 受控命令审计条目（Ops Control Plane MVP 的"操作审计"；04 §6.2 的 MVP
// 映射：operatorId ≙ source 组件 id，target ≙ 聚合侧 nodeId，result ≙
// accepted+ok；requestId 待 RuntimeInvocation 协议层支持后补）。
// execute 尝试与协议层/策略层拒绝各记一条；snapshot 只读不记（避免噪声）。
// 自 MachineDaemon.h 上提（与 NodeReport.h 同法）：daemon 生产与中心聚合
// 共用同一数据形状。
struct AuditEntry {
    std::chrono::system_clock::time_point timestamp{};
    runtime::ComponentId source = 0;
    std::string command;   // 未知方法请求记 method 名
    std::string args;
    bool accepted = false;  // false = 拒绝（畸形载荷/空命令/策略门/未知方法）
    bool ok = false;        // agent 执行结果（accepted 时有意义）
};

// 节点归属的审计记录：daemon 上报本机条目时打上 nodeId 归属（跨机汇入
// 中心后按节点可查）。审计是历史事实：节点注销/掉线摘除不影响其留存，
// 存储上界由中心环形容量约束。
struct NodeAuditEntry {
    std::string nodeId;
    AuditEntry entry{};
};

// 审计上报出口：MachineDaemon 只依赖本接口（不认识具体控制面中心），
// 生产接 OpsControlCenter 的可查询审计环形缓冲（04 §8 MVP"操作审计"）。
class INodeAuditSink {
public:
    virtual ~INodeAuditSink() = default;  // LCOV_EXCL_LINE C++ ABI：trivial 虚析构是空体，gcc 不为其生成计数指令，D0/D1/D2 三符号变体恒 0（结构不可测）

    virtual void publish(const NodeAuditEntry& entry) = 0;
};

}  // namespace theseed::control::machine
