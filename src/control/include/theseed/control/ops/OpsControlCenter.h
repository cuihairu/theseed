#pragma once

#include "theseed/control/machine/AuditEntry.h"
#include "theseed/control/machine/NodeReport.h"

#include <chrono>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace theseed::control::ops {

// 最小控制面中心聚合器（设计文档 04-ops-control-plane MVP 的”转发聚合”基座；
// 06 §2.4 的节点摘要上报落地端 + §8 MVP”操作审计”的汇聚端）：
//
// - 注册：registerNode 上线占位（早于首份快照即可查询可见；已知节点只刷新
//   lastSeen——注册不是快照，不得覆盖已有 summary）；
// - ingest：按 nodeId upsert 每节点最新快照（每机一行，后到覆盖）；
// - 容量上界：节点数超过 maxNodes 时逐出最旧接入的节点（注册与首报共用
//   同一接入序，防失控节点撑爆中心——06 §1 “machine 不能变成监控中心”
//   的内存面镜像）；
// - 摘除：deregister 优雅下线立即摘；pruneStale 在中心 tick 里按 TTL
//   摘掉超时未上报的节点（疑似掉线兜底，与注销语义自洽）；
// - 审计：publish(NodeAuditEntry) 汇入节点归属审计环形（容量
//   maxAuditEntries，满后丢最旧，0 = 关闭审计聚合）；auditTrail 按 nodeId
//   可查，时间升序；审计是历史事实，不随节点注销/摘除而清。
// - 查询：latest（单节点）、snapshotNodes（全量、按 nodeId 稳定排序）。
//
// 跨机网络转发（多台机器汇到一台中心）属跨 realm 异步平面，后续接入；
// 本类先保证聚合语义可单测、可被 MachineAgent::report() 直推。
class OpsControlCenter final : public machine::INodeReportSink,
                               public machine::INodeAuditSink {
public:
    struct Config {
        std::size_t maxNodes = 256;  // 0 = 不设上界（测试/单机内嵌场景）
        std::size_t maxAuditEntries = 1024;  // 审计环形容量；0 = 关闭审计聚合
    };

    // 注：`Config config = {}` 的类内默认实参对带 NSDMI 的嵌套类型非法
    //（gcc：default member initializer required before the end of its
    // enclosing class），默认配置走无参重载（与 LocalHostProbe 同法）。
    OpsControlCenter();
    explicit OpsControlCenter(Config config);

    // 上线注册：未知节点建占位行（summary 为空 = 已注册未报快照）；
    // 已知节点退化为心跳——只续 lastSeen，快照原样保留。
    void registerNode(const std::string& nodeId,
                      std::chrono::system_clock::time_point now) override;

    void publish(const machine::NodeReport& report) override;

    // 优雅下线摘除；返回是否确有该节点。审计等历史数据不受注销影响。
    bool deregister(const std::string& nodeId) override;

    // 审计汇入（INodeAuditSink 落地端，04 §8"操作审计"）：追加进节点归属
    // 审计环形，容量 maxAuditEntries，满后丢最旧；0 = 关闭审计聚合。
    // 无 nodeId 的记录与节点上报同一身份纪律：丢弃。
    void publish(const machine::NodeAuditEntry& entry) override;

    // 单节点最新快照；无该节点返回 false。
    bool latest(const std::string& nodeId, machine::NodeReport& out) const;

    // 全量节点快照，按 nodeId 升序（输出稳定，快照语义）。
    std::vector<machine::NodeReport> snapshotNodes() const;

    std::size_t nodeCount() const;

    // 摘掉 lastUpdate 距 now 超过 ttl 的节点，返回摘除数量。
    std::size_t pruneStale(std::chrono::milliseconds ttl,
                           std::chrono::system_clock::time_point now);

    // 审计流水查询：时间升序（= 追加序）的拷贝；可按节点过滤。
    std::vector<machine::NodeAuditEntry> auditTrail() const;
    std::vector<machine::NodeAuditEntry> auditTrail(
        const std::string& nodeId) const;
    std::size_t auditCount() const;

private:
    void evictOldestIfFull();

    Config config_;
    std::unordered_map<std::string, machine::NodeReport> nodes_;
    // 逐出顺序：按节点首报时间（非最后更新时间）——活跃节点不应因持续
    // 上报而被反复移到队尾，容量压力应优先挤掉“最早接入”的节点。
    std::vector<std::string> insertionOrder_;
    // 审计环形：daemon 单线程顺序推入，追加序即时间序；满后丢最旧。
    // 节点注销/掉线摘除不清审计——审计是历史事实，上界由容量约束。
    std::vector<machine::NodeAuditEntry> auditEntries_;
};

}  // namespace theseed::control::ops
