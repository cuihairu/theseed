#include "theseed/runtime/TickDiagnostics.h"

#include "theseed/foundation/Logger.h"
#include "theseed/foundation/Metrics.h"

namespace theseed::runtime {

namespace {

// 遥测命名与控制面同族（snake_case + _count）：慢 tick 是 ops 决策信号
// （宿主机过载 / 脚本卡顿的第一手证据）。
constexpr const char* kSlowTickCount = "slow_tick_count";

}  // namespace

TickDiagnostics::TickDiagnostics() : TickDiagnostics(Config{}) {}

TickDiagnostics::TickDiagnostics(Config config) : config_(config) {}

void TickDiagnostics::onTickCompleted(std::uint64_t tickIndex, Duration duration) {
    // 毫秒口径与调度器直方图一致；阈值比较在毫秒尺度做（亚毫秒抖动
    // 不影响整数阈值附近的判定）。
    const auto durationMs =
        std::chrono::duration<double, std::milli>(duration).count();
    const auto thresholdMs = static_cast<double>(config_.slowThreshold.count());
    if (durationMs <= thresholdMs) {
        return;  // 预算内：正常 tick 不计数不扰日志
    }

    ++slowTickCount_;
    lastSlowTickIndex_ = tickIndex;
    foundation::MetricsRegistry::instance().counter(kSlowTickCount).increment();

    // 命名属性单行化：gcc 会把多行调用表达式的计数错归因到续行
    const auto tickValue = static_cast<std::int64_t>(tickIndex);
    const foundation::LogAttribute tickAttr = {"tick_index", tickValue};
    const foundation::LogAttribute durationAttr = {"duration_ms", durationMs};
    const foundation::LogAttribute thresholdAttr = {"threshold_ms", thresholdMs};
    foundation::logWarn("runtime.slow_tick",
                        {tickAttr, durationAttr, thresholdAttr});
}

std::uint64_t TickDiagnostics::slowTickCount() const {
    return slowTickCount_;
}

std::uint64_t TickDiagnostics::lastSlowTickIndex() const {
    return lastSlowTickIndex_;
}

}  // namespace theseed::runtime
