// OpsControlCenter 测试：节点摘要聚合语义（upsert/容量逐出/TTL 摘除/排序
// 快照）+ MachineAgent::report 出口链路 + MachineDaemon 周期上报（真实
// agent 采样推给中心，首个 tick 立即上报）。
#include "theseed/control/machine/MachineAgent.h"
#include "theseed/control/machine/MachineDaemon.h"
#include "theseed/control/machine/NodeReport.h"
#include "theseed/control/ops/OpsControlCenter.h"
#include "theseed/foundation/Metrics.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

using theseed::control::machine::AccessRole;
using theseed::control::machine::HostSummary;
using theseed::control::machine::IHostProbe;
using theseed::control::machine::INodeReportSink;
using theseed::control::machine::IProcessSupervisor;
using theseed::control::machine::MachineAgent;
using theseed::control::machine::MachineDaemon;
using theseed::control::machine::NodeAuditEntry;
using theseed::control::machine::NodeProfileArtifact;
using theseed::control::machine::NodeReport;
using theseed::control::machine::NodeSummary;
using theseed::control::machine::ProfileMeta;
using theseed::control::machine::ProfileQuery;
using theseed::control::machine::ProcessSummary;
using theseed::control::ops::OpsControlCenter;
namespace foundation = theseed::foundation;

int testsPassed = 0;
int testsFailed = 0;

#define TEST(name)                                            \
    do {                                                      \
        std::cout << "  " << (name) << "... " << std::flush;  \
    } while (0)

#define PASS()                                                \
    do {                                                      \
        std::cout << "OK\n";                                  \
        ++testsPassed;                                        \
    } while (0)

#define FAIL(msg)                                             \
    do {                                                      \
        std::cout << "FAILED: " << (msg) << "\n";             \
        ++testsFailed;                                        \
    } while (0)

#define EXPECT(cond, msg)                                     \
    do {                                                      \
        if (!(cond)) FAIL(msg);                               \
    } while (0)

namespace {

NodeReport makeReport(const std::string& nodeId, double cpuUsage,
                      std::chrono::system_clock::time_point timestamp) {
    NodeReport report;
    report.nodeId = nodeId;
    report.timestamp = timestamp;
    report.summary.host.hostname = nodeId;
    report.summary.host.cpuUsage = cpuUsage;
    return report;
}

// 审计条目假件：顺序即断言对象（追加序 = 时间序），时间戳不参与。
NodeAuditEntry makeAudit(const std::string& nodeId, const std::string& command) {
    NodeAuditEntry entry;
    entry.nodeId = nodeId;
    entry.entry.command = command;
    return entry;
}

// 固定 hostname 的探针假件：验证 nodeId 取自快照 hostname。
class FixedHostProbe final : public IHostProbe {
public:
    HostSummary sample() override {
        HostSummary summary;
        summary.hostname = "node-alpha";
        summary.platform = "test";
        return summary;
    }
};

// 异常探针：hostname 为空（nodeId 身份口径下不可注册）。
class EmptyHostProbe final : public IHostProbe {
public:
    HostSummary sample() override { return {}; }
};

class FixedSupervisor final : public IProcessSupervisor {
public:
    std::vector<ProcessSummary> listProcesses() const override { return {}; }
    bool start(const std::string&) override { return true; }
    bool stop(std::uint32_t) override { return true; }
    bool restart(std::uint32_t) override { return true; }
    bool terminateUnmanaged(std::uint32_t) override { return false; }
};

// 捕获假件：记录中心出口推过的注册/上报/注销。
class CapturingSink final : public INodeReportSink {
public:
    void registerNode(const std::string& nodeId,
                      std::chrono::system_clock::time_point) override {
        registered.push_back(nodeId);
    }
    void publish(const NodeReport& report) override { published.push_back(report); }
    bool deregister(const std::string& nodeId) override {
        deregistered.push_back(nodeId);
        return true;
    }
    std::vector<std::string> registered;
    std::vector<NodeReport> published;
    std::vector<std::string> deregistered;
};

}  // namespace

int main() {
    std::cout << "OpsControlCenterTest:\n";

    const auto baseTime = std::chrono::system_clock::now();

    TEST("publish upserts by nodeId keeping the latest");
    {
        OpsControlCenter center;
        center.publish(makeReport("node-a", 10.0, baseTime));
        center.publish(makeReport("node-b", 20.0, baseTime));
        center.publish(makeReport("node-a", 15.0, baseTime + std::chrono::seconds{5}));

        EXPECT(center.nodeCount() == 2, "duplicate nodeId must upsert");
        NodeReport out;
        EXPECT(center.latest("node-a", out), "node-a must exist");
        EXPECT(out.summary.host.cpuUsage == 15.0, "latest snapshot must win");
        EXPECT(out.timestamp == baseTime + std::chrono::seconds{5},
               "upsert must refresh timestamp");
        PASS();
    }

    TEST("publish without nodeId is dropped");
    {
        OpsControlCenter center;
        center.publish(makeReport("", 10.0, baseTime));
        EXPECT(center.nodeCount() == 0, "identity-less reports cannot aggregate");
        PASS();
    }

    TEST("latest returns false for unknown node");
    {
        OpsControlCenter center;
        NodeReport out;
        EXPECT(!center.latest("ghost", out), "unknown node must miss");
        PASS();
    }

    TEST("snapshotNodes is sorted by nodeId");
    {
        OpsControlCenter center;
        center.publish(makeReport("node-c", 3.0, baseTime));
        center.publish(makeReport("node-a", 1.0, baseTime));
        center.publish(makeReport("node-b", 2.0, baseTime));

        const auto nodes = center.snapshotNodes();
        EXPECT(nodes.size() == 3, "all nodes present");
        EXPECT(nodes[0].nodeId == "node-a" && nodes[1].nodeId == "node-b" &&
                   nodes[2].nodeId == "node-c",
               "output must be nodeId-ordered");
        PASS();
    }

    TEST("capacity eviction removes the earliest-registered node");
    {
        OpsControlCenter::Config config;
        config.maxNodes = 2;
        OpsControlCenter center(config);
        center.publish(makeReport("early", 1.0, baseTime));
        center.publish(makeReport("mid", 2.0, baseTime));
        // early 持续上报刷新：首报序不因活跃而变，容量压力仍挤掉它
        center.publish(makeReport("early", 1.5, baseTime + std::chrono::seconds{1}));
        center.publish(makeReport("late", 3.0, baseTime));

        NodeReport out;
        EXPECT(!center.latest("early", out), "earliest-registered node evicted");
        EXPECT(center.latest("mid", out) && out.summary.host.cpuUsage == 2.0,
               "mid survives");
        EXPECT(center.latest("late", out), "late survives");
        PASS();
    }

    TEST("pruneStale removes only timed-out nodes");
    {
        OpsControlCenter center;
        const auto staleTime = baseTime - std::chrono::seconds{120};
        center.publish(makeReport("stale", 1.0, staleTime));
        center.publish(makeReport("fresh", 2.0, baseTime));

        const auto pruned = center.pruneStale(std::chrono::seconds{60}, baseTime);
        EXPECT(pruned == 1, "only the stale node is pruned");
        NodeReport out;
        EXPECT(!center.latest("stale", out), "stale node gone");
        EXPECT(center.latest("fresh", out), "fresh node kept");
        PASS();
    }

    TEST("capacity eviction still correct after pruneStale");
    {
        OpsControlCenter::Config config;
        config.maxNodes = 2;
        OpsControlCenter center(config);
        center.publish(
            makeReport("first", 1.0, baseTime - std::chrono::seconds{99}));
        center.publish(makeReport("second", 2.0, baseTime));

        EXPECT(center.pruneStale(std::chrono::seconds{10}, baseTime) == 1,
               "first pruned by ttl");
        center.publish(makeReport("third", 3.0, baseTime));
        center.publish(makeReport("fourth", 4.0, baseTime));

        NodeReport out;
        EXPECT(center.latest("fourth", out), "fourth present");
        // 容量 2：prune 掉 first 后幸存的 second 最早，进 third/fourth 时被挤出；
        // 若首报序残留未清理，逐出目标会错乱（漏逐或误逐 third）
        EXPECT(!center.latest("second", out), "earliest survivor evicted");
        EXPECT(center.latest("third", out), "third survives");
        EXPECT(center.latest("fourth", out), "fourth survives");
        EXPECT(center.nodeCount() == 2, "capacity respected after prune");
        PASS();
    }

    TEST("registerNode creates a placeholder visible before any snapshot");
    {
        OpsControlCenter center;
        center.registerNode("node-r", baseTime);

        NodeReport out;
        EXPECT(center.nodeCount() == 1, "registration inserts a row");
        EXPECT(center.latest("node-r", out), "registered node is queryable");
        EXPECT(out.nodeId == "node-r", "identity recorded");
        EXPECT(out.summary.host.hostname.empty(),
               "placeholder has no snapshot yet");
        EXPECT(out.timestamp == baseTime, "lastSeen starts at registration");
        PASS();
    }

    TEST("registerNode on known node refreshes lastSeen without clobbering snapshot");
    {
        OpsControlCenter center;
        center.publish(makeReport("node-a", 10.0, baseTime));
        center.registerNode("node-a", baseTime + std::chrono::seconds{3});

        NodeReport out;
        EXPECT(center.latest("node-a", out), "node still present");
        EXPECT(out.summary.host.cpuUsage == 10.0, "snapshot preserved");
        EXPECT(out.timestamp == baseTime + std::chrono::seconds{3},
               "lastSeen advanced");
        PASS();
    }

    TEST("registerNode drops empty identity");
    {
        OpsControlCenter center;
        center.registerNode("", baseTime);
        EXPECT(center.nodeCount() == 0, "identity discipline same as publish");
        PASS();
    }

    TEST("deregister removes the node and reports presence");
    {
        OpsControlCenter center;
        center.publish(makeReport("node-a", 1.0, baseTime));
        EXPECT(center.deregister("node-a"), "existing node deregisters true");

        NodeReport out;
        EXPECT(!center.latest("node-a", out), "node removed");
        EXPECT(center.nodeCount() == 0, "no residue");
        EXPECT(center.deregister("node-a") == false,
               "double deregister misses");
        EXPECT(center.deregister("ghost") == false,
               "unknown node deregister misses");
        PASS();
    }

    TEST("eviction stays correct after deregister");
    {
        OpsControlCenter::Config config;
        config.maxNodes = 2;
        OpsControlCenter center(config);
        center.publish(makeReport("first", 1.0, baseTime));
        center.publish(makeReport("second", 2.0, baseTime));
        // 注销摘节点也清接入序残留；重新填满后逐出仍瞄准正确的幸存者
        EXPECT(center.deregister("first"), "deregistered");
        center.publish(makeReport("third", 3.0, baseTime));
        center.publish(makeReport("fourth", 4.0, baseTime));

        NodeReport out;
        EXPECT(center.nodeCount() == 2, "capacity respected");
        EXPECT(!center.latest("second", out), "earliest survivor evicted");
        EXPECT(center.latest("third", out), "third survives");
        EXPECT(center.latest("fourth", out), "fourth survives");
        PASS();
    }

    TEST("registered but silent node is pruned by ttl");
    {
        OpsControlCenter center;
        center.registerNode("quiet", baseTime - std::chrono::seconds{120});
        center.registerNode("loud", baseTime);
        center.publish(makeReport("loud", 5.0, baseTime));  // 上报续住 lastSeen

        EXPECT(center.pruneStale(std::chrono::seconds{60}, baseTime) == 1,
               "only the silent registration times out");
        NodeReport out;
        EXPECT(!center.latest("quiet", out), "silent placeholder pruned");
        EXPECT(center.latest("loud", out), "reporting node survives");
        PASS();
    }

    TEST("daemon lifecycle registers at start and deregisters at stop");
    {
        OpsControlCenter center;
        MachineAgent agent(std::make_unique<FixedHostProbe>(),
                           std::make_unique<FixedSupervisor>());
        MachineDaemon::Config config;
        config.listenPort = 0;
        config.reportInterval = std::chrono::milliseconds{0};
        config.reportSink = &center;
        MachineDaemon daemon(config, agent);

        EXPECT(daemon.start(), "daemon start");
        EXPECT(center.nodeCount() == 1, "start registers machine identity");
        NodeReport out;
        EXPECT(center.latest("node-alpha", out),
               "nodeId = snapshot hostname even with interval 0");

        EXPECT(daemon.start(), "second start idempotent");
        EXPECT(center.nodeCount() == 1, "re-register is a heartbeat, not a row");

        daemon.stop();
        EXPECT(center.nodeCount() == 0, "graceful stop deregisters immediately");
        daemon.stop();
        EXPECT(center.nodeCount() == 0, "double stop stays deregistered");
        PASS();
    }

    TEST("empty hostname never registers");
    {
        OpsControlCenter center;
        MachineAgent agent(std::make_unique<EmptyHostProbe>(),
                           std::make_unique<FixedSupervisor>());
        MachineDaemon::Config config;
        config.listenPort = 0;
        config.reportSink = &center;
        MachineDaemon daemon(config, agent);

        EXPECT(daemon.start(), "daemon start");
        EXPECT(center.nodeCount() == 0, "identity-less host cannot register");
        daemon.stop();
        EXPECT(center.nodeCount() == 0, "stop without registration is a no-op");
        PASS();
    }

    TEST("MachineAgent::report pushes snapshot with nodeId from hostname");
    {
        CapturingSink sink;
        MachineAgent agent(std::make_unique<FixedHostProbe>(),
                           std::make_unique<FixedSupervisor>(), &sink);
        agent.report();

        EXPECT(sink.published.size() == 1, "one report per report() call");
        EXPECT(sink.published[0].nodeId == "node-alpha",
               "nodeId must come from snapshot hostname");
        EXPECT(sink.published[0].summary.host.hostname == "node-alpha",
               "report carries the full snapshot");
        PASS();
    }

    TEST("audit publish aggregates with node attribution");
    {
        OpsControlCenter center;
        center.publish(makeAudit("node-a", "start"));
        center.publish(makeAudit("node-a", "stop"));
        center.publish(makeAudit("node-b", "restart"));

        EXPECT(center.auditCount() == 3, "all entries aggregated");
        const auto trail = center.auditTrail();
        EXPECT(trail.size() == 3, "full trail");
        EXPECT(trail[0].entry.command == "start" &&
                   trail[1].entry.command == "stop" &&
                   trail[2].nodeId == "node-b",
               "trail is chronological with attribution");
        const auto onlyA = center.auditTrail("node-a");
        EXPECT(onlyA.size() == 2 && onlyA[1].entry.command == "stop",
               "per-node filter keeps order");
        EXPECT(center.auditTrail("ghost").empty(),
               "unknown node has no trail");
        PASS();
    }

    TEST("audit ring evicts oldest beyond capacity");
    {
        OpsControlCenter::Config config;
        config.maxAuditEntries = 2;
        OpsControlCenter center(config);
        center.publish(makeAudit("node-a", "one"));
        center.publish(makeAudit("node-a", "two"));
        center.publish(makeAudit("node-a", "three"));

        const auto trail = center.auditTrail();
        EXPECT(trail.size() == 2, "ring bounded");
        EXPECT(trail[0].entry.command == "two" &&
                   trail[1].entry.command == "three",
               "oldest audit dropped first");
        PASS();
    }

    TEST("audit aggregation gated by capacity 0 and identity");
    {
        OpsControlCenter::Config off;
        off.maxAuditEntries = 0;
        OpsControlCenter silent(off);
        silent.publish(makeAudit("node-a", "start"));
        EXPECT(silent.auditCount() == 0, "capacity 0 disables aggregation");

        OpsControlCenter center;
        center.publish(makeAudit("", "start"));
        EXPECT(center.auditCount() == 0, "identity-less entries dropped");
        PASS();
    }

    TEST("audit trail survives node deregistration and pruning");
    {
        OpsControlCenter center;
        center.publish(makeReport("node-a", 1.0, baseTime));
        center.publish(makeAudit("node-a", "start"));

        center.deregister("node-a");
        EXPECT(center.nodeCount() == 0, "node deregistered");
        EXPECT(center.auditCount() == 1, "audit is history, not node state");

        center.pruneStale(std::chrono::seconds{60}, baseTime);
        EXPECT(center.auditCount() == 1, "pruning leaves audit intact");
        PASS();
    }

    TEST("center telemetry: node gauge, prune and audit-drop counters");
    {
        // 指标为进程级单例：跨用例累积，按增量断言
        auto& nodeGauge = foundation::MetricsRegistry::instance().gauge(
            "ops_nodes_registered");
        auto& pruned = foundation::MetricsRegistry::instance().counter(
            "ops_nodes_pruned_count");
        auto& dropped = foundation::MetricsRegistry::instance().counter(
            "ops_audit_dropped_count");
        const auto gauge0 = nodeGauge.value();
        const auto pruned0 = pruned.value();
        const auto dropped0 = dropped.value();

        OpsControlCenter::Config config;
        config.maxAuditEntries = 1;
        OpsControlCenter center(config);
        center.registerNode("node-a", baseTime);
        EXPECT(nodeGauge.value() == gauge0 + 1, "gauge tracks registration");
        center.publish(makeReport("node-a", 1.0, baseTime));
        EXPECT(nodeGauge.value() == gauge0 + 1, "upsert keeps roster size");
        center.publish(makeAudit("node-a", "one"));
        center.publish(makeAudit("node-a", "two"));
        EXPECT(dropped.value() == dropped0 + 1, "ring overflow counted");

        center.publish(makeReport("node-b", 2.0,
                                  baseTime - std::chrono::seconds{120}));
        EXPECT(nodeGauge.value() == gauge0 + 2,
               "first report joins the roster");
        center.pruneStale(std::chrono::seconds{60}, baseTime);
        EXPECT(pruned.value() == pruned0 + 1, "prune counter incremented");
        EXPECT(nodeGauge.value() == gauge0 + 1, "gauge reflects the prune");

        center.deregister("node-a");
        EXPECT(nodeGauge.value() == gauge0, "gauge tracks deregistration");
        PASS();
    }

    TEST("MachineAgent::report without sink is a no-op");
    {
        MachineAgent agent(std::make_unique<FixedHostProbe>(),
                           std::make_unique<FixedSupervisor>());
        agent.report();  // 未注册出口：空操作不崩溃

        CapturingSink sink;
        agent.setReportSink(&sink);
        agent.report();
        agent.setReportSink(nullptr);
        agent.report();
        EXPECT(sink.published.size() == 1, "sink detach stops reporting");
        PASS();
    }

    TEST("daemon periodic reporting feeds the control center");
    {
        OpsControlCenter center;
        MachineAgent agent(std::make_unique<FixedHostProbe>(),
                           std::make_unique<FixedSupervisor>(), &center);

        MachineDaemon::Config config;
        config.listenPort = 0;
        config.reportInterval = std::chrono::milliseconds{30};
        config.reportSink = &center;  // daemon 周期上报的出口
        MachineDaemon daemon(config, agent);
        if (!daemon.start()) {
            FAIL("daemon start failed");
            return testsFailed == 0 ? 0 : 1;
        }

        // 首个 tick 立即上报；跑 ~100ms 应有多次 upsert（nodeCount 恒 1）
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
        while (std::chrono::steady_clock::now() < deadline) {
            daemon.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        NodeReport out;
        EXPECT(center.nodeCount() == 1, "single-node host upserts in place");
        EXPECT(center.latest("node-alpha", out), "center holds the node");
        EXPECT(out.summary.host.hostname == "node-alpha", "snapshot intact");
        EXPECT(out.summary.host.platform == "test", "sampled payload intact");
        daemon.stop();
        PASS();
    }

    TEST("interval 0 still registers but never publishes snapshots");
    {
        OpsControlCenter center;
        MachineAgent agent(std::make_unique<FixedHostProbe>(),
                           std::make_unique<FixedSupervisor>(), &center);

        MachineDaemon::Config off;
        off.listenPort = 0;
        off.reportInterval = std::chrono::milliseconds{0};
        off.reportSink = &center;
        MachineDaemon daemonOff(off, agent);
        if (!daemonOff.start()) {
            FAIL("daemon start failed");
            return testsFailed == 0 ? 0 : 1;
        }
        for (int i = 0; i < 5; ++i) {
            daemonOff.tick();
        }
        // 注册是生命周期语义（不随上报节奏）；快照缺席 = 占位行仍空
        NodeReport out;
        EXPECT(center.nodeCount() == 1,
               "lifecycle registration is cadence-independent");
        EXPECT(center.latest("node-alpha", out) &&
                   out.summary.host.hostname.empty(),
               "no snapshot published at interval 0");
        daemonOff.stop();
        EXPECT(center.nodeCount() == 0, "stop deregisters the placeholder");

        // 有周期、无 sink：不采样不注册，中心毫无痕迹
        MachineDaemon::Config noSink;
        noSink.listenPort = 0;
        noSink.reportInterval = std::chrono::milliseconds{1};
        MachineDaemon daemonNoSink(noSink, agent);
        if (!daemonNoSink.start()) {
            FAIL("daemon start failed");
            return testsFailed == 0 ? 0 : 1;
        }
        for (int i = 0; i < 5; ++i) {
            daemonNoSink.tick();
        }
        EXPECT(center.nodeCount() == 0, "missing sink disables reporting");
        daemonNoSink.stop();
        PASS();
    }

    TEST("profile aggregation: dimension queries are honest about gaps");
    {
        OpsControlCenter::Config cfg;
        cfg.maxProfileArtifacts = 8;
        cfg.profilePolicy.canAccess = {1};
        cfg.roleBindings = {{1, AccessRole::Admin}};
        OpsControlCenter center(cfg);

        // 两台 agent 的剖面元数据经 report() 通道汇聚：node-a 只带进程维
        // （机器 agent 的 TickProfiler 是进程级 tick 粒度，无 entity 维
        // 生产者）；node-b 的一条实体维条目只为验证中心过滤逻辑本身。
        auto reportA = makeReport("node-a", 1.0, std::chrono::system_clock::now());
        ProfileMeta metaA;
        metaA.handle = 5;
        metaA.tickCount = 32;
        metaA.windowMs = 1.5;
        reportA.profiles.push_back(metaA);
        center.publish(reportA);

        auto reportB = makeReport("node-b", 2.0, std::chrono::system_clock::now());
        ProfileMeta metaB;
        metaB.handle = 7;
        metaB.entityType = "Monster";
        reportB.profiles.push_back(metaB);
        center.publish(reportB);

        ProfileQuery allQuery;
        auto all = center.queryProfiles(1, allQuery);
        EXPECT(all.size() == 2, "both nodes' profiles aggregated, got " +
                                    std::to_string(all.size()));
        EXPECT(all[0].nodeId == "node-a" && all[0].meta.handle == 5,
               "output stable by (nodeId, handle)");
        EXPECT(all[1].nodeId == "node-b" && all[1].meta.handle == 7,
               "second row is node-b");

        ProfileQuery byNode;
        byNode.nodeId = "node-a";
        auto byNodeRows = center.queryProfiles(1, byNode);
        EXPECT(byNodeRows.size() == 1 && byNodeRows[0].meta.handle == 5,
               "node dimension filters");

        ProfileQuery byEntity;
        byEntity.entityId = "e-1";
        EXPECT(center.queryProfiles(1, byEntity).empty(),
               "entity dimension has no producers: honest empty");

        ProfileQuery byType;
        byType.entityType = "Monster";
        auto byTypeRows = center.queryProfiles(1, byType);
        EXPECT(byTypeRows.size() == 1 && byTypeRows[0].nodeId == "node-b",
               "entity-type dimension filters reported metas");

        ProfileQuery byNope;
        byNope.nodeId = "nope";
        EXPECT(center.queryProfiles(1, byNope).empty(),
               "unknown node is honestly empty");

        // 未授权查询：空结果 + 拒绝审计（nodeId 空 = 中心本地动作）+ 指标
        auto& queryAccepted = foundation::MetricsRegistry::instance().counter(
            "center_profile_query_accepted_count");
        auto& queryRejected = foundation::MetricsRegistry::instance().counter(
            "center_profile_query_rejected_count");
        const auto acc0 = queryAccepted.value();
        const auto rej0 = queryRejected.value();
        const auto audits0 = center.auditCount();

        EXPECT(center.queryProfiles(2, allQuery).empty(),
               "unauthorized query is empty");
        EXPECT(queryRejected.value() == rej0 + 1, "rejected counter +1");
        const auto trail = center.auditTrail();
        EXPECT(trail.size() == audits0 + 1, "rejection audited");
        EXPECT(trail.back().nodeId.empty(), "center-local audit: empty nodeId");
        EXPECT(trail.back().entry.command == "center.profiler.query" &&
                   !trail.back().entry.accepted,
               "rejection entry shape");

        center.queryProfiles(1, allQuery);
        EXPECT(queryAccepted.value() == acc0 + 1, "accepted counter +1");
        EXPECT(center.auditCount() == audits0 + 2,
               "acceptance audited in the same ring");
        PASS();
    }

    TEST("artifact relay: center stores copies, download reads them back");
    {
        OpsControlCenter::Config cfg;
        cfg.maxProfileArtifacts = 2;
        cfg.profilePolicy.canAccess = {1};
        cfg.roleBindings = {{1, AccessRole::Admin}};
        OpsControlCenter center(cfg);

        const auto mk = [](const std::string& node, std::uint64_t handle,
                           const std::string& payload) {
            NodeProfileArtifact frame;
            frame.nodeId = node;
            frame.meta.handle = handle;
            frame.meta.tickCount = 4;
            frame.payload = payload;
            return frame;
        };

        center.publish(mk("node-a", 1, "{\"handle\":1}"));
        center.publish(mk("node-a", 2, "{\"handle\":2}"));

        // 产物帧同时并入查询索引（报告通道未及的窗口也能查到）
        ProfileQuery byNode;
        byNode.nodeId = "node-a";
        EXPECT(center.queryProfiles(1, byNode).size() == 2,
               "artifact frames merge into the query index");

        std::string out;
        EXPECT(center.downloadProfileArtifact(1, "node-a", 2, out),
               "download by (node, handle)");
        EXPECT(out == "{\"handle\":2}", "payload is the relayed bytes");

        const auto audits0 = center.auditCount();
        EXPECT(!center.downloadProfileArtifact(1, "node-a", 99, out),
               "unknown handle is honestly refused");
        EXPECT(!center.downloadProfileArtifact(1, "node-b", 1, out),
               "unknown node is honestly refused");
        EXPECT(!center.downloadProfileArtifact(2, "node-a", 1, out),
               "unauthorized download refused");
        const auto trail = center.auditTrail();
        EXPECT(trail.size() == audits0 + 3, "three attempts audited");
        EXPECT(!trail[trail.size() - 3].entry.accepted,
               "unknown-handle attempt rejected");
        EXPECT(!trail[trail.size() - 2].entry.accepted,
               "unknown-node attempt rejected");
        EXPECT(trail[trail.size() - 1].entry.command ==
                   "center.profiler.download",
               "download command shape");

        // 副本环形容量 2：第三份挤掉最旧（到达序），逐出计数
        auto& dropped = foundation::MetricsRegistry::instance().counter(
            "center_profile_artifacts_dropped_count");
        const auto drop0 = dropped.value();
        center.publish(mk("node-a", 3, "{\"handle\":3}"));
        EXPECT(dropped.value() == drop0 + 1, "eviction counted");
        EXPECT(!center.downloadProfileArtifact(1, "node-a", 1, out),
               "oldest copy evicted");
        EXPECT(center.downloadProfileArtifact(1, "node-a", 3, out),
               "newest copy held");

        // 空帧：身份纪律丢弃（不聚合、不进索引、不计数）
        center.publish(mk("", 9, "x"));
        ProfileQuery allQuery;
        EXPECT(center.queryProfiles(1, allQuery).size() == 3,
               "empty frame contributes nothing");
        PASS();
    }

    TEST("profile index lifecycle: store off, node removal, snapshot overwrite");
    {
        // 副本存储关闭：元数据索引照常，下载如实报无副本
        OpsControlCenter::Config cfg;
        cfg.maxProfileArtifacts = 0;
        cfg.profilePolicy.canAccess = {1};
        cfg.roleBindings = {{1, AccessRole::Admin}};
        OpsControlCenter center(cfg);

        auto report = makeReport("node-a", 1.0, std::chrono::system_clock::now());
        ProfileMeta meta;
        meta.handle = 5;
        report.profiles.push_back(meta);
        center.publish(report);

        NodeProfileArtifact frame;
        frame.nodeId = "node-a";
        frame.meta.handle = 5;
        frame.payload = "bytes";
        center.publish(frame);  // 字节不留（存储关闭），索引并入句柄

        ProfileQuery byNodeA;
        byNodeA.nodeId = "node-a";
        EXPECT(center.queryProfiles(1, byNodeA).size() == 1,
               "index works without copies");
        std::string out;
        EXPECT(!center.downloadProfileArtifact(1, "node-a", 5, out),
               "no local copy: honest refusal");

        // 报告快照后到覆盖：无剖面的报告清空该节点索引
        center.publish(makeReport("node-a", 1.0, std::chrono::system_clock::now()));
        EXPECT(center.queryProfiles(1, byNodeA).empty(),
               "profile-free report clears the index (snapshot semantics)");

        // 仅产物帧的节点（不在名册）也可查询；注销名册节点清索引
        NodeProfileArtifact frameB;
        frameB.nodeId = "node-b";
        frameB.meta.handle = 1;
        frameB.payload = "b1";
        center.publish(frameB);
        ProfileQuery byNodeB;
        byNodeB.nodeId = "node-b";
        EXPECT(center.queryProfiles(1, byNodeB).size() == 1,
               "artifact-only node is queryable");
        EXPECT(center.deregister("node-a"), "node-a deregistered");
        EXPECT(center.queryProfiles(1, byNodeA).empty(),
               "deregister clears the index (state semantics)");

        // pruneStale 同纪律：摘节点清索引
        auto staleReport = makeReport(
            "node-c", 1.0, std::chrono::system_clock::now() - std::chrono::hours{1});
        ProfileMeta metaC;
        metaC.handle = 2;
        staleReport.profiles.push_back(metaC);
        center.publish(staleReport);
        EXPECT(center.pruneStale(std::chrono::milliseconds{1000},
                                 std::chrono::system_clock::now()) == 1,
               "stale node pruned");
        ProfileQuery byNodeC;
        byNodeC.nodeId = "node-c";
        EXPECT(center.queryProfiles(1, byNodeC).empty(),
               "prune clears the index");

        // 容量逐出同样清索引（maxNodes=1，n1 被 n2 挤出）
        OpsControlCenter::Config tightCfg;
        tightCfg.maxNodes = 1;
        tightCfg.profilePolicy.canAccess = {1};
        tightCfg.roleBindings = {{1, AccessRole::Admin}};
        OpsControlCenter tight(tightCfg);
        auto r1 = makeReport("n1", 1.0, std::chrono::system_clock::now());
        ProfileMeta m1;
        m1.handle = 1;
        r1.profiles.push_back(m1);
        tight.publish(r1);
        tight.publish(makeReport("n2", 2.0, std::chrono::system_clock::now()));
        ProfileQuery byN1;
        byN1.nodeId = "n1";
        EXPECT(tight.queryProfiles(1, byN1).empty(),
               "capacity eviction clears the index");
        PASS();
    }

    TEST("center role tiers: profile reads need bound roles above canAccess (04 §6.1)");
    {
        // §6.1 叠位语义：canAccess 四者皆含，角色表只绑三个——三档角色
        // 都能读（inspect 面，ReadOnly 及以上），未绑定者被角色门拒绝
        // （canAccess 放行 ≠ 可读：角色位是第二道独立关口）。
        OpsControlCenter::Config cfg;
        cfg.profilePolicy.canAccess = {1, 2, 3, 4};
        cfg.roleBindings = {{1, AccessRole::ReadOnly},
                            {2, AccessRole::Operator},
                            {3, AccessRole::Admin}};
        OpsControlCenter center(cfg);

        auto report = makeReport("node-a", 1.0, std::chrono::system_clock::now());
        ProfileMeta meta;
        meta.handle = 9;
        report.profiles.push_back(meta);
        center.publish(report);

        ProfileQuery byNode;
        byNode.nodeId = "node-a";
        EXPECT(center.queryProfiles(1, byNode).size() == 1, "ReadOnly reads");
        EXPECT(center.queryProfiles(2, byNode).size() == 1, "Operator reads");
        EXPECT(center.queryProfiles(3, byNode).size() == 1, "Admin reads");

        auto& queryRejected = foundation::MetricsRegistry::instance().counter(
            "center_profile_query_rejected_count");
        auto& downloadRejected =
            foundation::MetricsRegistry::instance().counter(
                "center_profile_download_rejected_count");
        const auto qRej0 = queryRejected.value();
        const auto dRej0 = downloadRejected.value();
        const auto audits0 = center.auditCount();

        EXPECT(center.queryProfiles(4, byNode).empty(),
               "unbound query is refused despite canAccess");
        std::string out;
        EXPECT(!center.downloadProfileArtifact(4, "node-a", 9, out),
               "unbound download is refused despite canAccess");

        EXPECT(queryRejected.value() == qRej0 + 1, "query rejected +1");
        EXPECT(downloadRejected.value() == dRej0 + 1, "download rejected +1");
        const auto& trail = center.auditTrail();
        EXPECT(trail.size() == audits0 + 2, "both refusals audited");
        EXPECT(trail[trail.size() - 2].nodeId.empty() &&
                   !trail[trail.size() - 2].entry.accepted,
               "query refusal: center-local rejected entry");
        EXPECT(trail[trail.size() - 1].entry.command ==
                   "center.profiler.download",
               "download refusal keeps its command shape");
        PASS();
    }

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed)
              << "\n";
    return testsFailed == 0 ? 0 : 1;
}
