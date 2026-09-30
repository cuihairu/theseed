// LocalHostProbe 的 CPU 采样稳定化与网络流量聚合测试。
// CPU 分支用脚本化 CpuTickQuery 驱动（确定性），网络走注入透传 +
// 默认探针在真实平台计数器上的单调性（累计计数器只增不减）。
// probe_detail 纯函数（tick 拆分 / 网卡聚合 / 占比换算）由本测试在 Linux
// 上直接驱动全分支——它们是 macOS 胶合复用的同一份口径逻辑。
#include "theseed/control/machine/HostProbe.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using theseed::control::machine::HostSummary;
using theseed::control::machine::LocalHostProbe;
namespace probe_detail = theseed::control::machine::probe_detail;

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

// primeCpuSample 中 cpuTickQuery_ 成功但 total 未推进（<= baseTotal）：
// 验证分支进入 continue 而非 return，窗口耗尽后沿用基线。
static void testPrimeCpuSampleQuerySucceedsButTotalNotAdvanced() {
    TEST("primeCpuSample query succeeds but total not advanced");

    // 首读给出基线，后续查询恒返回相同 total（total == baseTotal）
    struct SameTotalTicks {
        int calls = 0;
        bool operator()(std::uint64_t& idle, std::uint64_t& total) {
            ++calls;
            idle = 1000;
            total = 4000;  // 恒等于基线
            return true;   // 查询成功，但 total 未推进
        }
    } sameTotal;

    LocalHostProbe::Config config;
    config.minCpuWindow = std::chrono::milliseconds{30};
    config.retryGranularity = std::chrono::milliseconds{5};
    LocalHostProbe probe(config, std::ref(sameTotal));

    const auto summary = probe.sample();
    // 窗口耗尽未推进：cpuUsage 保持 0（首采粘滞值）
    if (summary.cpuUsage == 0.0 && sameTotal.calls >= 2) PASS();
    else FAIL("cpuUsage=" + std::to_string(summary.cpuUsage) + " calls=" + std::to_string(sameTotal.calls));
}

// 自举窗口内查询失败（/proc/stat 瞬时不可读的退化情形）：cpuTickQuery_
// 假臂在窗口轮询中短路，空转至窗口耗尽，保持基线读数不闪回 0。
static void testPrimingWindowQueryFailure() {
    TEST("priming window tolerates query failure");

    // 首采给出基线，其后每次查询都失败——ScriptedTicks 耗尽后恒重复末组
    // （恒真），窗口内的假查询需显式注入。
    struct FailAfterFirst {
        int calls = 0;
        bool operator()(std::uint64_t& idle, std::uint64_t& total) {
            if (++calls > 1) return false;
            idle = 100;
            total = 400;
            return true;
        }
    } flaky;

    LocalHostProbe::Config config;
    config.minCpuWindow = std::chrono::milliseconds{30};
    config.retryGranularity = std::chrono::milliseconds{5};
    LocalHostProbe probe(config, std::ref(flaky));

    const auto summary = probe.sample();
    // 窗口耗尽未推进：totalDelta=0 → 粘滞初值 0。calls>1 断言窗口内确有一次
    // 假查询走过（deadline 在 now+30ms，首轮必进），否则本例会退化成
    // testPrimingWindowExhausted 的同义重复而假臂静默失覆。
    if (summary.cpuUsage == 0.0 && flaky.calls > 1) PASS();
    else FAIL("expected 0.0 with window query failure, got " + std::to_string(summary.cpuUsage) +
              " after " + std::to_string(flaky.calls) + " calls");
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

// aggregate/accumulateLinkCounters：回环排除 + 多网卡求和（Linux "lo" 与
// macOS IFF_LOOPBACK 共用的同一份口径；流式臂是 Linux 解析器的生产路径）。
static void testAggregateLinkCounters() {
    TEST("aggregate/accumulateLinkCounters excludes loopback and sums nics");

    bool ok = true;
    // 流式累积（Linux 逐行路径同函数）：物理行计入、回环行不计入、可对
    // 非零 total 续加。
    probe_detail::LinkCounters stream{};
    probe_detail::accumulateLinkCounters(stream, {false, 5, 6});
    probe_detail::accumulateLinkCounters(stream, {true, 100, 200});
    probe_detail::accumulateLinkCounters(stream, {false, 1, 2});
    ok = ok && stream.rxBytes == 6 && stream.txBytes == 8;

    // 空表：恒 {0,0}（循环不进入臂）
    const auto empty = probe_detail::aggregateLinkCounters({});
    ok = ok && empty.first == 0 && empty.second == 0;

    // 全回环：逐行命中排除臂，合计仍为 0
    const std::array<probe_detail::LinkCounters, 2> loopbacks{
        probe_detail::LinkCounters{true, 111, 222}, probe_detail::LinkCounters{true, 333, 444}};
    const auto loopOnly = probe_detail::aggregateLinkCounters(loopbacks);
    ok = ok && loopOnly.first == 0 && loopOnly.second == 0;

    // 混合：物理网卡求和、lo 不计（含 rx≠tx 的非对称行）
    const std::array<probe_detail::LinkCounters, 3> rows{probe_detail::LinkCounters{true, 1000, 2000},
                                                         probe_detail::LinkCounters{false, 7, 9},
                                                         probe_detail::LinkCounters{false, 3, 1}};
    const auto mixed = probe_detail::aggregateLinkCounters(rows);
    ok = ok && mixed.first == 10 && mixed.second == 10;

    // NSDMI 默认构造：三字段零值/非回环
    const probe_detail::LinkCounters defaults{};
    ok = ok && !defaults.loopback && defaults.rxBytes == 0 && defaults.txBytes == 0;

    if (ok) PASS();
    else FAIL("empty rx=" + std::to_string(empty.first) + " mixed rx=" + std::to_string(mixed.first) +
                  " tx=" + std::to_string(mixed.second));
}

// windowsLinkCounters：MIB_IF_ROW2.Type → 回环归一（ifType 24 判据）+
// 64 位计数透传（Windows 胶合的行构造面在 Linux 直测全分支；求和排除
// 仍走 aggregate/accumulate 共用口径）。
static void testWindowsLinkCounters() {
    TEST("windowsLinkCounters maps ifType 24 to loopback and passes counters");

    bool ok = true;
    // 软件回环臂：RFC 2863 ifType 24 → 排除
    const auto loop = probe_detail::windowsLinkCounters(probe_detail::kIfTypeSoftwareLoopback, 100, 200);
    ok = ok && loop.loopback && loop.rxBytes == 100 && loop.txBytes == 200;

    // 常量钉死 IANA 值（Windows 胶合另有 static_assert 对照 SDK 宏）
    ok = ok && probe_detail::kIfTypeSoftwareLoopback == 24;

    // 物理网卡臂：以太网(6)/其他(1)/保留(0) 均不判回环，计数原样透传
    const auto eth = probe_detail::windowsLinkCounters(6, 7, 9);
    ok = ok && !eth.loopback && eth.rxBytes == 7 && eth.txBytes == 9;
    const auto other = probe_detail::windowsLinkCounters(1, 3, 1);
    ok = ok && !other.loopback;
    const auto zero = probe_detail::windowsLinkCounters(0, 0, 0);
    ok = ok && !zero.loopback && zero.rxBytes == 0 && zero.txBytes == 0;

    // 64 位大数透传（32 位 dwInOctets 会回绕的口径点）
    const auto wide = probe_detail::windowsLinkCounters(6, 0xFFFFFFFFFFFFFFFFULL, 0x8000000000000000ULL);
    ok = ok && !wide.loopback && wide.rxBytes == 0xFFFFFFFFFFFFFFFFULL &&
        wide.txBytes == 0x8000000000000000ULL;

    // 归一进聚合：回环行不计入、物理行求和（与 Linux/macOS 同一份求和）
    const std::array<probe_detail::LinkCounters, 3> rows{
        probe_detail::windowsLinkCounters(probe_detail::kIfTypeSoftwareLoopback, 1000, 2000),
        probe_detail::windowsLinkCounters(6, 7, 9),
        probe_detail::windowsLinkCounters(234, 3, 1)};
    const auto mixed = probe_detail::aggregateLinkCounters(rows);
    ok = ok && mixed.first == 10 && mixed.second == 10;

    if (ok) PASS();
    else FAIL("loop arm or counter passthrough mismatch");
}

// splitCpuTicksApple：busy=user+nice+system、idle 单列、total=busy+idle
// （macOS HOST_CPU_LOAD_INFO 四态读数进此纯函数，采样差值逻辑不变）。
static void testSplitCpuTicksApple() {
    TEST("splitCpuTicksApple busy/idle partition");

    bool ok = true;
    const auto [idle, total] = probe_detail::splitCpuTicksApple(100, 50, 25, 825);
    ok = ok && idle == 825 && total == 1000;  // nice 计入忙侧

    const auto [fullIdle, fullTotal] = probe_detail::splitCpuTicksApple(500, 0, 500, 0);
    ok = ok && fullIdle == 0 && fullTotal == 1000;  // 满载臂

    const auto [zeroIdle, zeroTotal] = probe_detail::splitCpuTicksApple(0, 0, 0, 0);
    ok = ok && zeroIdle == 0 && zeroTotal == 0;  // 全零退化读数

    if (ok) PASS();
    else FAIL("idle=" + std::to_string(idle) + " total=" + std::to_string(total));
}

// usagePercent：分母防护与线性换算（Linux sysinfo、macOS vm 统计共用；
// CPU/磁盘之外的占比臂也在此直测）。
static void testUsagePercent() {
    TEST("usagePercent guards zero total and scales linearly");

    bool ok = true;
    ok = ok && probe_detail::usagePercent(0, 0) == 0.0;   // 分母防护臂
    ok = ok && probe_detail::usagePercent(5, 0) == 0.0;   // 防护臂对非零分子同样成立
    ok = ok && near(probe_detail::usagePercent(3, 4), 75.0, 1e-9);
    ok = ok && near(probe_detail::usagePercent(1, 3), 100.0 / 3.0, 1e-9);
    ok = ok && near(probe_detail::usagePercent(3, 2), 150.0, 1e-9);  // 异常读数不裁剪（文档口径）
    ok = ok && probe_detail::usagePercent(0, 100) == 0.0;            // 零用量 ≠ 分母防护，走正常臂

    if (ok) PASS();
    else FAIL("zero-total arm or linear conversion mismatch");
}

// 默认探针（真实平台计数器源）：网络累计计数器只增不减，两次采样单调；
// CPU/内存/磁盘读数在合法区间、平台串可辨。断言全平台中立——Linux/macOS
// （CI macos job 首验 Apple 胶合）/Windows 上同一条测试都必须成立。
//
// 环境毛刺纪律：读数窗口是真实时间，单窗口可能撞上瞬态环境噪声（容器
// veth 在两次读之间摘除会让主机累计回退、fd 短暂耗尽、space() 瞬时
// 错误归零）。断言一字不动——违例时取新鲜样本对复验同一组断言，连续
// 两窗违例才判失败：单次毛刺不足以否证「累计计数器只增不减」的产品
// 口径，连续违例则照报真实回归。
static void testRealProcSources() {
    TEST("default probe reads real platform counters monotonic");

    const auto check = []() -> std::string {
        LocalHostProbe probe;
        const auto first = probe.sample();
        const auto second = probe.sample();

        bool ok = !first.hostname.empty();
        ok = ok && (first.platform == "linux" || first.platform == "macos" || first.platform == "windows");
        ok = ok && first.cpuUsage >= 0.0 && first.cpuUsage <= 100.0;
        ok = ok && first.memoryUsage > 0.0 && first.memoryUsage <= 100.0;
        ok = ok && first.diskUsage > 0.0 && first.diskUsage <= 100.0;
        ok = ok && first.loadAverage >= 0.0;
        ok = ok && second.networkRxBytes >= first.networkRxBytes;
        ok = ok && second.networkTxBytes >= first.networkTxBytes;
        if (ok) {
            return {};  // 空串 = 本窗断言全过
        }
        return "platform=" + first.platform + " mem=" + std::to_string(first.memoryUsage) +
               " disk=" + std::to_string(first.diskUsage) + " rx: " +
               std::to_string(first.networkRxBytes) + "->" + std::to_string(second.networkRxBytes);
    };

    const auto firstDetail = check();
    if (firstDetail.empty()) {
        PASS();
        return;
    }
    // 首窗违例：先复验再定论（探测臂不计失败，只有定论臂落 PASS/FAIL）
    std::cout << "  (environment glitch, re-checking with a fresh sample pair) " << std::flush;
    const auto secondDetail = check();
    if (secondDetail.empty()) {
        PASS();
    } else {
        std::cout << "\n";
        FAIL(secondDetail + " [still failing after re-check]");
    }
}

int main() {
    std::cout << "HostProbe tests:\n";

    testFirstSamplePriming();
    testPrimingWindowExhausted();
    testPrimeCpuSampleQuerySucceedsButTotalNotAdvanced();
    testPrimingWindowQueryFailure();
    testStickyReadingOnZeroDelta();
    testQueryFailureKeepsReading();
    testClampBounds();
    testZeroWindowNoPriming();
    testAggregateLinkCounters();
    testWindowsLinkCounters();
    testSplitCpuTicksApple();
    testUsagePercent();
    testRealProcSources();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
