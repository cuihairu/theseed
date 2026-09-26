#pragma once

#include "theseed/control/machine/AuditEntry.h"
#include "theseed/control/machine/NodeReport.h"
#include "theseed/control/machine/ProfileRelay.h"

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
//   可查，时间升序；审计是历史事实，不随节点注销/摘除而清。中心侧
//   自身动作（剖面查询/下载及其拒绝）入同一环形，nodeId 为空 = 中心
//   本地动作（与"daemon 无身份上报丢弃"纪律区分：后者管的是无法归属
//   的节点上报，中心本地动作的身份就是中心本身）。
// - 剖面（04 §7 中心侧）：report() 通道快照汇入每节点剖面元数据索引
//   （后到覆盖，随节点摘除而清——状态语义）；publish(NodeProfileArtifact)
//   推送产物副本进全局环形（容量 maxProfileArtifacts，满后丢最旧，
//   0 = 关闭副本存储；历史事实语义，不随节点摘除而清，与审计同纪律）。
//   queryProfiles / downloadProfileArtifact 是中心侧读入口，沿用
//   DiagnosticsPolicy 读位（canAccess）鉴权，全部尝试（含拒绝）入审计
//   与指标；触发权（canTrigger）仍在 agent 侧，中心无写命令。
// - 查询：latest（单节点）、snapshotNodes（全量、按 nodeId 稳定排序）。
//
// 跨机网络转发（多台机器汇到一台中心）属跨 realm 异步平面，后续接入；
// 本类先保证聚合语义可单测、可被 MachineAgent::report() 直推。
class OpsControlCenter final : public machine::INodeReportSink,
                               public machine::INodeAuditSink,
                               public machine::INodeArtifactSink {
public:
    struct Config {
        std::size_t maxNodes = 256;  // 0 = 不设上界（测试/单机内嵌场景）
        std::size_t maxAuditEntries = 1024;  // 审计环形容量；0 = 关闭审计聚合
        // 中心侧产物副本环形容量（跨节点全局计）；0 = 关闭副本存储
        // （元数据索引照常工作，下载如实报无副本）。
        std::size_t maxProfileArtifacts = 32;
        // 中心侧剖面读入口授权（DiagnosticsPolicy 读位，沿用 agent 侧
        // 口径；canTrigger 写位在中心无生效点——本批无中心侧写命令）。
        // 缺省空集 = 查询/下载一律拒绝（安全缺省）。
        machine::DiagnosticsPolicy profilePolicy;
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

    // 产物副本汇入（INodeArtifactSink 落地端，04 §7 中心持有副本）：
    // 追加进全局副本环形（跨节点统一容量，满后丢最旧，0 = 关闭副本
    // 存储）；元数据同时并入该节点的剖面索引（句柄未知则追加——报告
    // 通道未及的窗口，查询侧也能看到）。无 nodeId 的帧按身份纪律丢弃。
    void publish(const machine::NodeProfileArtifact& artifact) override;

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

    // 中心侧剖面查询（04 §7"按 entity / entity type / process 查询当前
    // 剖面"，只读）：filter 字段为空 = 该维度不过滤；process 维 ≙ nodeId。
    // entity/entityType 两维当前无生产者（机器 agent 的 TickProfiler 是
    // 进程级 tick 粒度），按这两维查询如实返回空——不编造数据。
    // 全部尝试（含拒绝）入审计（command = center.profiler.query）与指标；
    // 未授权（profilePolicy.canAccess 不含 requester）返回空并照记拒绝。
    // 调用方身份是进程内自报（正式鉴权归 Gateway 层，见 todo.md 边界）。
    // 非常量：每次尝试（含拒绝）都要落审计环形——"只读"是授权语义，
    // 物理上审计留痕本身是写。
    std::vector<machine::NodeProfileEntry> queryProfiles(
        runtime::ComponentId requester, const machine::ProfileQuery& query);

    // 中心侧产物下载（只读）：从中心副本环形取 (nodeId, handle) 的只读
    // 字节——不再依赖直连该 agent。副本未持有（未推送/已逐出/副本存储
    // 关闭）如实返回 false。全部尝试（含拒绝）入审计
    // （command = center.profiler.download）与指标。非常量理由同上。
    bool downloadProfileArtifact(runtime::ComponentId requester,
                                 const std::string& nodeId,
                                 std::uint64_t handle,
                                 std::string& out);

private:
    void evictOldestIfFull();
    // 审计环形落账（容量 + 丢弃计数 + 水位同步）。节点归属纪律
    //（空 nodeId 丢弃）由 publish(NodeAuditEntry) 把守；中心本地动作
    //（nodeId 为空）直接走本函数入账。
    void appendAuditRing(const machine::NodeAuditEntry& entry);
    bool isProfileAccessAuthorized(runtime::ComponentId requester) const;

    Config config_;
    std::unordered_map<std::string, machine::NodeReport> nodes_;
    // 逐出顺序：按节点首报时间（非最后更新时间）——活跃节点不应因持续
    // 上报而被反复移到队尾，容量压力应优先挤掉“最早接入”的节点。
    std::vector<std::string> insertionOrder_;
    // 审计环形：daemon 单线程顺序推入，追加序即时间序；满后丢最旧。
    // 节点注销/掉线摘除不清审计——审计是历史事实，上界由容量约束。
    std::vector<machine::NodeAuditEntry> auditEntries_;
    // 每节点剖面元数据索引（快照语义：report() 通道后到覆盖；随节点
    // 摘除而清——与快照同属状态）。
    std::unordered_map<std::string, std::vector<machine::ProfileMeta>>
        profileIndex_;
    // 产物副本环形（跨节点全局计，到达序；历史事实语义，不随节点
    // 摘除而清——与审计同纪律，上界由容量约束）。
    std::vector<machine::NodeProfileArtifact> profileArtifacts_;
};

}  // namespace theseed::control::ops
