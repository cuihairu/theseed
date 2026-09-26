// OpsControlCenter 测试：节点摘要聚合语义（upsert/容量逐出/TTL 摘除/排序
// 快照）+ MachineAgent::report 出口链路 + MachineDaemon 周期上报（真实
// agent 采样推给中心，首个 tick 立即上报）。
#include "theseed/control/machine/MachineAgent.h"
#include "theseed/control/machine/MachineDaemon.h"
#include "theseed/control/machine/NodeReport.h"
#include "theseed/control/ops/OpsControlCenter.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

using theseed::control::machine::HostSummary;
using theseed::control::machine::IHostProbe;
using theseed::control::machine::INodeReportSink;
using theseed::control::machine::IProcessSupervisor;
using theseed::control::machine::MachineAgent;
using theseed::control::machine::MachineDaemon;
using theseed::control::machine::NodeReport;
using theseed::control::machine::NodeSummary;
using theseed::control::machine::ProcessSummary;
using theseed::control::ops::OpsControlCenter;

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

class FixedSupervisor final : public IProcessSupervisor {
public:
    std::vector<ProcessSummary> listProcesses() const override { return {}; }
    bool start(const std::string&) override { return true; }
    bool stop(std::uint32_t) override { return true; }
    bool restart(std::uint32_t) override { return true; }
};

// 捕获假件：记录 report 推送过的报告。
class CapturingSink final : public INodeReportSink {
public:
    void publish(const NodeReport& report) override { published.push_back(report); }
    std::vector<NodeReport> published;
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

    TEST("daemon without interval or sink never reports");
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
        EXPECT(center.nodeCount() == 0, "interval 0 disables reporting");
        daemonOff.stop();

        // 有周期、无 sink：同样静默
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

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed)
              << "\n";
    return testsFailed == 0 ? 0 : 1;
}
