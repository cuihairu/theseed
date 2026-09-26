// TickDiagnostics 单元测试：慢 tick 分类边界（低于/恰好等于/超出阈值）、
// 计数与结构化警告日志联动、只读视图语义；指标为进程级单例，按增量断言。
#include "theseed/runtime/TickDiagnostics.h"

#include "theseed/foundation/Logger.h"
#include "theseed/foundation/Metrics.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace theseed::runtime;
namespace foundation = theseed::foundation;

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

// 捕获假件：单线程测试上下文写日志，无需加锁（与调度器线程纪律一致）。
class CapturingLogger final : public foundation::ILogger {
public:
    void log(foundation::LogRecord record) override {
        records_.push_back(std::move(record));
    }
    void setLevel(foundation::LogLevel level) override { level_ = level; }
    foundation::LogLevel level() const override { return level_; }

    std::vector<foundation::LogRecord> drain() { return std::move(records_); }

private:
    foundation::LogLevel level_ = foundation::LogLevel::Debug;
    std::vector<foundation::LogRecord> records_;
};

Duration ms(double value) {
    return std::chrono::duration_cast<Duration>(
        std::chrono::duration<double, std::milli>{value});
}

}  // namespace

int main() {
    std::cout << "TickDiagnosticsTest:" << std::endl;

    TEST("ticks within budget stay silent");
    {
        TickDiagnostics diagnostics(
            TickDiagnostics::Config{std::chrono::milliseconds{200}});
        const auto slow0 =
            foundation::MetricsRegistry::instance().counter("slow_tick_count")
                .value();

        diagnostics.onTickCompleted(0, ms(199.999));
        // 恰好等于阈值：超预算才是信号，等于不算
        diagnostics.onTickCompleted(1, ms(200));
        if (diagnostics.slowTickCount() != 0)
            FAIL("below/at threshold must not count");
        if (foundation::MetricsRegistry::instance()
                .counter("slow_tick_count")
                .value() != slow0)
            FAIL("counter must stay untouched for fast ticks");
        PASS();
    }

    TEST("slow ticks count, log, and update the read-only view");
    {
        auto& counter =
            foundation::MetricsRegistry::instance().counter("slow_tick_count");
        const auto slow0 = counter.value();

        const auto captured = std::make_shared<CapturingLogger>();
        auto previousLogger = foundation::takeGlobalLogger();
        foundation::setGlobalLogger(captured);

        TickDiagnostics diagnostics(
            TickDiagnostics::Config{std::chrono::milliseconds{200}});
        diagnostics.onTickCompleted(7, ms(321.5));
        diagnostics.onTickCompleted(8, ms(250));
        diagnostics.onTickCompleted(9, ms(100));  // 回落：不再累加

        if (diagnostics.slowTickCount() != 2) FAIL("slow tick count");
        if (diagnostics.lastSlowTickIndex() != 8)
            FAIL("last slow tick index must track the latest slow tick");
        if (counter.value() != slow0 + 2) FAIL("slow_tick_count counter +2");

        const auto records = captured->drain();
        if (records.size() != 2) FAIL("one warning per slow tick");
        if (records[0].message != "runtime.slow_tick") FAIL("warning message");
        bool sawIndex = false;
        bool sawDuration = false;
        bool sawThreshold = false;
        for (const auto& attr : records[0].attrs) {
            if (attr.key == "tick_index" &&
                std::get<std::int64_t>(attr.value) == 7)
                sawIndex = true;
            if (attr.key == "duration_ms" &&
                std::get<double>(attr.value) == 321.5)
                sawDuration = true;
            if (attr.key == "threshold_ms" &&
                std::get<double>(attr.value) == 200)
                sawThreshold = true;
        }
        if (!sawIndex || !sawDuration || !sawThreshold)
            FAIL("warning must carry tick_index/duration_ms/threshold_ms");

        foundation::setGlobalLogger(std::move(previousLogger));
        PASS();
    }

    TEST("default config uses the 200ms threshold");
    {
        TickDiagnostics diagnostics;
        diagnostics.onTickCompleted(0, ms(199));
        if (diagnostics.slowTickCount() != 0)
            FAIL("default threshold must pass fast ticks");
        diagnostics.onTickCompleted(1, ms(201));
        if (diagnostics.slowTickCount() != 1)
            FAIL("default threshold must flag over-budget ticks");
        PASS();
    }

    TEST("scheduler seam feeds diagnostics without false slow ticks");
    {
        TickScheduler scheduler(std::chrono::milliseconds{0});
        TickDiagnostics diagnostics(
            TickDiagnostics::Config{std::chrono::milliseconds{10000}});
        scheduler.setObserver(&diagnostics);
        scheduler.runOnce();
        scheduler.runOnce();
        if (diagnostics.slowTickCount() != 0)
            FAIL("generous threshold must keep healthy ticks silent");
        scheduler.setObserver(nullptr);
        PASS();
    }

    std::cout << "\nAll TickDiagnostics tests passed!" << std::endl;
    return 0;
}
