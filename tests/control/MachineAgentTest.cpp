// MachineAgent 单元测试：snapshot 聚合 host/process 数据，
// execute 对 start/stop/restart 的分发与 pid 解析失败、未知命令分支。
#include "theseed/control/machine/MachineAgent.h"
#include "theseed/control/machine/HostProbe.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace theseed::control::machine;

#define TEST(name)                            \
    do {                                      \
        std::cout << "  " << name << "... ";  \
    } while (0)
#define PASS() std::cout << "OK" << std::endl
#define FAIL(msg)                                    \
    do {                                             \
        std::cout << "FAILED: " << msg << std::endl; \
        return 1;                                    \
    } while (0)

namespace {

class FakeHostProbe final : public IHostProbe {
public:
    HostSummary sample() override {
        HostSummary s;
        s.hostname = "fake-host";
        s.platform = "test";
        s.cpuUsage = 12.5;
        s.memoryUsage = 40.0;
        return s;
    }
};

class FakeSupervisor final : public IProcessSupervisor {
public:
    std::vector<ProcessSummary> listProcesses() const override {
        ProcessSummary p;
        p.name = "cellapp";
        p.pid = 4321;
        p.port = 20002;
        p.version = "0.1.0";
        p.healthy = true;
        p.managed = true;
        return {p};
    }

    bool start(const std::string& target) override {
        lastStart = target;
        return !target.empty();
    }

    bool stop(std::uint32_t pid) override {
        lastStop = pid;
        return pid != 0;
    }

    bool restart(std::uint32_t pid) override {
        lastRestart = pid;
        return pid != 0;
    }

    std::string lastStart;
    std::uint32_t lastStop = 0;
    std::uint32_t lastRestart = 0;
};

}  // namespace

int main() {
    std::cout << "MachineAgentTest:" << std::endl;

    auto probe = std::make_unique<FakeHostProbe>();
    auto supervisor = std::make_unique<FakeSupervisor>();
    auto* supervisorPtr = supervisor.get();
    MachineAgent agent(std::move(probe), std::move(supervisor));

    // snapshot 聚合 host 采样与进程列表
    TEST("snapshot aggregates host and processes");
    {
        const auto summary = agent.snapshot();
        if (summary.host.hostname != "fake-host") FAIL("host not sampled");
        if (summary.host.cpuUsage != 12.5) FAIL("cpu usage mismatch");
        if (summary.processes.size() != 1) FAIL("process list mismatch");
        if (summary.processes[0].name != "cellapp") FAIL("process name mismatch");
        PASS();
    }

    // start：参数原样转发，空目标走监督器的失败分支
    TEST("execute start forwards target");
    {
        if (!agent.execute("start", "cellapp")) FAIL("start should succeed");
        if (supervisorPtr->lastStart != "cellapp") FAIL("start target not forwarded");
        if (agent.execute("start", "")) FAIL("empty target should fail");
        PASS();
    }

    // stop：合法 pid 转发；非数字 / 尾随垃圾 / 空串解析失败
    TEST("execute stop parses pid");
    {
        if (!agent.execute("stop", "4321")) FAIL("stop should succeed");
        if (supervisorPtr->lastStop != 4321) FAIL("stop pid not forwarded");
        if (agent.execute("stop", "abc")) FAIL("non-numeric pid should fail");
        if (agent.execute("stop", "42x")) FAIL("trailing garbage should fail");
        if (agent.execute("stop", "")) FAIL("empty pid should fail");
        PASS();
    }

    // restart：合法 pid 转发；负数 / 溢出 / 空串解析失败
    TEST("execute restart parses pid");
    {
        if (!agent.execute("restart", "4322")) FAIL("restart should succeed");
        if (supervisorPtr->lastRestart != 4322) FAIL("restart pid not forwarded");
        if (agent.execute("restart", "-1")) FAIL("negative pid should fail");
        if (agent.execute("restart", "99999999999")) FAIL("overflow pid should fail");
        if (agent.execute("restart", "")) FAIL("empty pid should fail");
        PASS();
    }

    // 未知命令返回 false
    TEST("execute rejects unknown command");
    {
        if (agent.execute("pause", "1")) FAIL("unknown command should fail");
        if (agent.execute("", "1")) FAIL("empty command should fail");
        PASS();
    }

    // LocalHostProbe：连续两次采样走 CPU delta 计算分支。
    // /proc/stat 粒度是 jiffy（10ms），间隔太短 totalDelta 为 0，delta 分支不触发。
    TEST("host probe samples cpu delta across two calls");
    {
        theseed::control::machine::LocalHostProbe probe;
        const auto first = probe.sample();
        if (first.hostname.empty()) FAIL("hostname empty");
        if (first.platform.empty()) FAIL("platform empty");
        std::this_thread::sleep_for(std::chrono::milliseconds{60});
        const auto second = probe.sample();
        if (second.cpuUsage < 0.0 || second.cpuUsage > 100.0) FAIL("cpu usage out of range");
        if (second.memoryUsage < 0.0 || second.memoryUsage > 100.0) FAIL("memory usage out of range");
        if (second.diskUsage < 0.0 || second.diskUsage > 100.0) FAIL("disk usage out of range");
        PASS();
    }

#ifndef _WIN32
    // LocalProcessSupervisor：真实 fork/exec 一个 sleep 子进程，
    // 走 start/snapshot(stop/restart) 主路径。fork 出的子进程走 _exit，
    // 其分支计数不会写 gcda，但父进程路径全部生效。
    TEST("local supervisor manages a real process");
    {
        LocalProcessSupervisor supervisor;
        if (supervisor.start("")) FAIL("empty target should fail");
        if (supervisor.start("   ")) FAIL("whitespace-only target should fail");

        if (!supervisor.start("sleep 5")) FAIL("start sleep");
        auto procs = supervisor.listProcesses();
        std::uint32_t pid = 0;
        for (const auto& p : procs) {
            if (p.managed && p.name == "sleep") {
                pid = p.pid;
                break;
            }
        }
        if (pid == 0) FAIL("managed sleep missing from listProcesses");

        // restart：找到命令行 → terminate → 重新 start
        if (!supervisor.restart(pid)) FAIL("restart sleep");

        procs = supervisor.listProcesses();
        std::uint32_t newPid = 0;
        for (const auto& p : procs) {
            if (p.managed && p.name == "sleep") {
                newPid = p.pid;
                break;
            }
        }
        if (newPid == 0) FAIL("restarted sleep missing");

        if (!supervisor.stop(newPid)) FAIL("stop sleep");
        if (supervisor.stop(newPid)) FAIL("second stop should fail");
        if (supervisor.restart(newPid)) FAIL("restart stopped pid should fail");
        PASS();
    }
#endif

    std::cout << "MachineAgentTest: all passed" << std::endl;
    return 0;
}
