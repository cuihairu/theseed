#pragma once

#include "theseed/control/machine/NodeReport.h"

#include <chrono>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace theseed::control::ops {

// 最小控制面中心聚合器（设计文档 04-ops-control-plane MVP 的“转发聚合”基座；
// 06 §2.4 的节点摘要上报落地端）：
//
// - ingest：按 nodeId upsert 每节点最新快照（每机一行，后到覆盖）；
// - 容量上界：节点数超过 maxNodes 时逐出最旧上报的节点（防失控节点撑爆
//   中心——06 §1 “machine 不能变成监控中心”的内存面镜像）；
// - pruneStale：中心 tick 里按 TTL 摘掉超时未上报的节点（疑似掉线）；
// - 查询：latest（单节点）、snapshotNodes（全量、按 nodeId 稳定排序）。
//
// 跨机网络转发（多台机器汇到一台中心）属跨 realm 异步平面，后续接入；
// 本类先保证聚合语义可单测、可被 MachineAgent::report() 直推。
class OpsControlCenter final : public machine::INodeReportSink {
public:
    struct Config {
        std::size_t maxNodes = 256;  // 0 = 不设上界（测试/单机内嵌场景）
    };

    // 注：`Config config = {}` 的类内默认实参对带 NSDMI 的嵌套类型非法
    //（gcc：default member initializer required before the end of its
    // enclosing class），默认配置走无参重载（与 LocalHostProbe 同法）。
    OpsControlCenter();
    explicit OpsControlCenter(Config config);

    void publish(const machine::NodeReport& report) override;

    // 单节点最新快照；无该节点返回 false。
    bool latest(const std::string& nodeId, machine::NodeReport& out) const;

    // 全量节点快照，按 nodeId 升序（输出稳定，快照语义）。
    std::vector<machine::NodeReport> snapshotNodes() const;

    std::size_t nodeCount() const;

    // 摘掉 lastUpdate 距 now 超过 ttl 的节点，返回摘除数量。
    std::size_t pruneStale(std::chrono::milliseconds ttl,
                           std::chrono::system_clock::time_point now);

private:
    void evictOldestIfFull();

    Config config_;
    std::unordered_map<std::string, machine::NodeReport> nodes_;
    // 逐出顺序：按节点首报时间（非最后更新时间）——活跃节点不应因持续
    // 上报而被反复移到队尾，容量压力应优先挤掉“最早接入”的节点。
    std::vector<std::string> insertionOrder_;
};

}  // namespace theseed::control::ops
