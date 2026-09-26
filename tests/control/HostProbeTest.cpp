// LocalHostProbe 的 CPU 采样稳定化与网络流量聚合测试。
// CPU 分支用脚本化 CpuTickQuery 驱动（确定性），网络走注入透传 +
// 默认探针在真实 /proc/net/dev 上的单调性（累计计数器只增不减）。
#include "theseed/control/machine/HostProbe.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using theseed::control::machine::HostSummary;
using theseed::control::machine::LocalHostProbe;

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name)                                           \
    do {                                                     \
        std::cout << "  " << (name) << "... " << std::flush; \
    } while (0)

#define PASS()                    \
    do {                          \
        std::cout << "OK\n";      \
        ++testsPassed;            \
    } while (0)

#define FAIL(msg)                                  \
    do {                                           \
        std::cout << "FAILED: " << (msg) << "\n";  \
        ++testsFailed;                             \
    } while (0)

namespace {

// 脚本化 tick 序列：每次调用弹出下一组 (idle, total)；序列耗尽后重复最后一组。
class ScriptedTicks {
public:
    void push(std::uint64_t idle, std::uint64_t total) { script_.emplace_back(idle, total); }

    bool operator()(std::uint64_t& idleTicks, std::uint64_t& totalTicks) {
        if (script_.empty()) return false;
        const auto index = next_ < script_.size() ? next_ : script_.size() - 1;
        idleTicks = script_[index].first;
        totalTicks = script_[index].second;
        ++next_;
        return true;
    }

private:
    std::vector<std::pair<std::uint64_t, std::uint64_t>> script_;
    std::size_t next_ = 0;
};

bool near(double value, double expected, double epsilon) { return value >= expected - epsilon && value <= expected + epsilon; }

}  // namespace

// 首采自举：窗口内 tick 推进后以窗口两端差值出读数，CLI 单次调用也有真实值。
static void testFirstSamplePriming() {
    TEST("first sample primes within window");

    ScriptedTicks ticks;
    ticks.push(1000, 4000);  // 首读数（基线）
    ticks.push(1100, 4200);  // 自举轮询的推进读数：idleΔ=100 totalΔ=200 → 50%

    LocalHostProbe::Config config;
    config.minCpuWindow = std::chrono::milliseconds{50};
    config.retryGranularity = std::chrono::milliseconds{5};
    LocalHostProbe probe(config, ticks, [] { return std::pair<std::uint64_t, std::uint64_t>{7, 9}; });

    const auto summary = probe.sample();
    if (near(summary.cpuUsage, 50.0, 0.01) && summary.networkRxBytes == 7 && summary.networkTxBytes == 9) PASS();
    else FAIL("cpuUsage=" + std::to_string(summary.cpuUsage));
}

// 自举窗口耗尽（tick 冻结的退化情形）：读数保持初值 0，不崩溃。
static void testPrimingWindowExhausted() {
    TEST("priming window exhausted keeps last reading");

    ScriptedTicks ticks;
    ticks.push(1000, 4000);  // 恒不推进

    LocalHostProbe::Config config;
    config.minCpuWindow = std::chrono::milliseconds{20};
    config.retryGranularity = std::chrono::milliseconds{5};
    LocalHostProbe probe(config, ticks, [] { return std::pair<std::uint64_t, std::uint64_t>{0, 0}; });

    const auto summary = probe.sample();
    if (summary.cpuUsage == 0.0) PASS();
    else FAIL("expected 0.0, got " + std::to_string(summary.cpuUsage));
}

// 粘滞读数：两次采样间 tick 零推进（窗口短于粒度）沿用上次读数，不闪回 0。
static void testStickyReadingOnZeroDelta() {
    TEST("zero-delta window keeps previous reading");

    ScriptedTicks ticks;
    ticks.push(100, 400);   // 首读（无基线，cpu=0）
    ticks.push(150, 500);   // idleΔ=50 totalΔ=100 → 50%
    ticks.push(150, 500);   // 零推进 → 保持 50%

    LocalHostProbe probe(LocalHostProbe::Config{}, ticks);
    probe.sample();
    const auto second = probe.sample();
    const auto third = probe.sample();
    if (near(second.cpuUsage, 50.0, 0.01) && near(third.cpuUsage, 50.0, 0.01)) PASS();
    else FAIL("second=" + std::to_string(second.cpuUsage) + " third=" + std::to_string(third.cpuUsage));
}

// 查询失败（如非 Linux 平台无 /proc/stat）：保留上次读数。
static void testQueryFailureKeepsReading() {
    TEST("query failure keeps last reading");

    ScriptedTicks ticks;
    ticks.push(100, 400);
    ticks.push(150, 500);
    // 第 3 次起：序列耗尽重复最后一组（150,500)——改为显式失败注入
    LocalHostProbe::Config config;
    config.minCpuWindow = std::chrono::milliseconds{0};

    // 前 two calls succeed, then fail
    struct Flaky {
        ScriptedTicks inner;
        int calls = 0;
        bool operator()(std::uint64_t& idle, std::uint64_t& total) {
            ++calls;
            if (calls >= 3) return false;
            return inner(idle, total);
        }
    } flaky;
    flaky.inner.push(100, 400);
    flaky.inner.push(150, 500);

    LocalHostProbe probe(config, std::ref(flaky));
    probe.sample();          // 基线
    const auto second = probe.sample();  // 50%
    const auto third = probe.sample();   // 查询失败 → 保持
    if (near(second.cpuUsage, 50.0, 0.01) && near(third.cpuUsage, 50.0, 0.01)) PASS();
    else FAIL("second=" + std::to_string(second.cpuUsage) + " third=" + std::to_string(third.cpuUsage));
}

// 满载与空载边界：clamp 到 [0,100]。
static void testClampBounds() {
    TEST("usage clamped to [0, 100]");

    {
        ScriptedTicks ticks;
        ticks.push(100, 400);
        ticks.push(100, 500);  // idleΔ=0 → 100%
        LocalHostProbe probe(LocalHostProbe::Config{}, ticks);
        probe.sample();
        const auto summary = probe.sample();
        if (!near(summary.cpuUsage, 100.0, 0.01)) { FAIL("expected 100, got " + std::to_string(summary.cpuUsage)); return; }
    }
    {
        ScriptedTicks ticks;
        ticks.push(100, 400);
        ticks.push(300, 600);  // idleΔ=totalΔ → 0%
        LocalHostProbe probe(LocalHostProbe::Config{}, ticks);
        probe.sample();
        const auto summary = probe.sample();
        if (!near(summary.cpuUsage, 0.0, 0.01)) { FAIL("expected 0, got " + std::to_string(summary.cpuUsage)); return; }
    }
    PASS();
}

// 窗口为 0 的首采：不自举（保持旧行为），第二次采样才有值。
static void testZeroWindowNoPriming() {
    TEST("zero window skips priming");

    ScriptedTicks ticks;
    ticks.push(100, 400);
    ticks.push(150, 500);

    LocalHostProbe::Config config;
    config.minCpuWindow = std::chrono::milliseconds{0};
    LocalHostProbe probe(config, ticks);
    const auto first = probe.sample();
    const auto second = probe.sample();
    if (first.cpuUsage == 0.0 && near(second.cpuUsage, 50.0, 0.01)) PASS();
    else FAIL("first=" + std::to_string(first.cpuUsage) + " second=" + std::to_string(second.cpuUsage));
}

// 默认探针（真实 /proc）：网络累计计数器只增不减，两次采样单调。
// CPU 值在 [0,100]；平台/主机名非空。
static void testRealProcSources() {
    TEST("default probe reads real /proc monotonic");

    LocalHostProbe probe;
    const auto first = probe.sample();
    const auto second = probe.sample();

    bool ok = !first.hostname.empty() && !first.platform.empty();
    ok = ok && first.cpuUsage >= 0.0 && first.cpuUsage <= 100.0;
    ok = ok && second.networkRxBytes >= first.networkRxBytes;
    ok = ok && second.networkTxBytes >= first.networkTxBytes;
    if (ok) PASS();
    else FAIL("rx: " + std::to_string(first.networkRxBytes) + "->" + std::to_string(second.networkRxBytes));
}

int main() {
    std::cout << "HostProbe tests:\n";

    testFirstSamplePriming();
    testPrimingWindowExhausted();
    testStickyReadingOnZeroDelta();
    testQueryFailureKeepsReading();
    testClampBounds();
    testZeroWindowNoPriming();
    testRealProcSources();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
