// TickProfiler 单元测试：固定窗口采样全生命周期——触发占号、限流拒绝、
// 窗口收满固化产物、环形逐出、未知句柄、无效配置；样本喂确定性耗时，
// 产物统计字段逐项断言。
#include "theseed/runtime/TickProfiler.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace theseed::runtime;

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

Duration ms(double value) {
    return std::chrono::duration_cast<Duration>(
        std::chrono::duration<double, std::milli>{value});
}

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

}  // namespace

int main() {
    std::cout << "TickProfilerTest:" << std::endl;

    TEST("window collects samples and finalizes an artifact");
    {
        TickProfiler::Config config;
        config.windowTicks = 4;
        config.maxArtifacts = 4;
        config.slowThreshold = std::chrono::milliseconds{200};
        TickProfiler profiler(config);

        // 空闲 tick：静默忽略，不进样本
        profiler.onTickCompleted(0, ms(500));

        const auto handle = profiler.trigger();
        if (handle == 0) FAIL("trigger must open a window");
        if (!profiler.sampling()) FAIL("sampling flag must be set");

        profiler.onTickCompleted(1, ms(100));
        profiler.onTickCompleted(2, ms(321.5));
        profiler.onTickCompleted(3, ms(99));
        profiler.onTickCompleted(4, ms(200));  // 恰好等于阈值：不算慢
        if (profiler.sampling()) FAIL("window must close when full");

        const auto artifacts = profiler.listArtifacts();
        if (artifacts.size() != 1) FAIL("one artifact expected");
        if (artifacts[0].handle != handle) FAIL("handle reserved at trigger");
        if (artifacts[0].tickCount != 4) FAIL("tick count mismatch");

        std::string payload;
        if (!profiler.artifactPayload(handle, payload))
            FAIL("payload must be downloadable by handle");
        if (!contains(payload, "\"handle\":" + std::to_string(handle)) ||
            !contains(payload, "\"window_ticks\":4") ||
            !contains(payload, "\"slow_threshold_ms\":200") ||
            !contains(payload, "\"slow_samples\":1") ||
            !contains(payload, "\"min_ms\":99") ||
            !contains(payload, "\"max_ms\":321.5") ||
            !contains(payload, "\"avg_ms\":180.125") ||
            !contains(payload, "\"samples\":[100,321.5,99,200]"))
            FAIL("payload statistics mismatch: " + payload);
        PASS();
    }

    TEST("retrigger while sampling is rate limited, invalid config refuses");
    {
        // 缺省构造：windowTicks=32 的默认口径可正常开窗
        TickProfiler profiler;
        const auto first = profiler.trigger();
        if (first == 0) FAIL("first trigger must succeed");
        if (profiler.trigger() != 0)
            FAIL("retrigger during an open window must be refused");

        TickProfiler::Config invalid;
        invalid.windowTicks = 0;
        TickProfiler broken(invalid);
        if (broken.trigger() != 0) FAIL("zero window config must refuse trigger");

        // 容量笔误（maxArtifacts=0）：0 视为 1，不静默关闭——窗口照开、
        // 产物照固化，环形只留最新一份。
        TickProfiler::Config zeroRing;
        zeroRing.windowTicks = 1;
        zeroRing.maxArtifacts = 0;
        TickProfiler ring(zeroRing);
        if (ring.trigger() == 0) FAIL("zero-capacity config must still trigger");
        ring.onTickCompleted(0, ms(5));
        if (ring.listArtifacts().size() != 1)
            FAIL("zero-capacity config must keep one artifact");
        PASS();
    }

    TEST("artifact ring evicts oldest and handles keep growing");
    {
        TickProfiler::Config config;
        config.windowTicks = 1;
        config.maxArtifacts = 2;
        TickProfiler profiler(config);

        const auto h1 = profiler.trigger();
        profiler.onTickCompleted(0, ms(10));
        const auto h2 = profiler.trigger();
        profiler.onTickCompleted(1, ms(20));
        const auto h3 = profiler.trigger();
        profiler.onTickCompleted(2, ms(30));
        if (h1 == 0 || h2 <= h1 || h3 <= h2) FAIL("handles must grow");

        const auto artifacts = profiler.listArtifacts();
        if (artifacts.size() != 2) FAIL("ring capacity must bound artifacts");
        if (artifacts[0].handle != h2 || artifacts[1].handle != h3)
            FAIL("oldest artifact must be evicted first");

        std::string out;
        if (profiler.artifactPayload(h1, out))
            FAIL("evicted handle must be gone");
        if (profiler.artifactPayload(99999, out))
            FAIL("unknown handle must fail");
        PASS();
    }

    std::cout << "\nAll TickProfiler tests passed!" << std::endl;
    return 0;
}
