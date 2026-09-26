#pragma once

#include "theseed/runtime/RuntimeTypes.h"
#include "theseed/runtime/TickScheduler.h"

#include <chrono>
#include <cstdint>

namespace theseed::runtime {

// 慢 tick 诊断（05-telemetry §6 Diagnostics Profiling 的 MVP 只读切片）：
// 把每次 tick 的实测耗时与阈值比较，超阈值累计 slow_tick_count 并打
// 结构化警告日志（tick 序号 / 实测耗时 / 阈值可查）——"这台机器的 tick
// 变慢了"成为可观测事实。
//
// 边界（避免诊断面职责漂移）：
// - tick_duration_ms 直方图与整 tick span 由 TickScheduler 自身观测
//   （遥测联动已落地），本类不重复观测耗时分布；
// - 实体级负载信号归 EntityLoadProfiler（04-runtime-profiler-and-load-
//   feedback 的谱系，喂负载均衡），与 tick 粒度"慢"判定互不替代；
// - 只读留痕，不触发任何控制动作——谁可以触发采样、谁可以下载结果，
//   归 04-ops-control-plane，后续接入。
//
// 注册：TickScheduler::setObserver(&diagnostics)。观察者不持有，建议在
// 调度线程启动前挂接；onTickCompleted 在 tick 线程内联回调，计数与
// 状态写入无锁（单线程纪律与 TickScheduler 一致），只读视图请同线程
// 读或外部自行同步。
class TickDiagnostics final : public ITickObserver {
public:
    struct Config final {
        // 慢 tick 阈值：实测耗时 > 阈值判慢（恰好等于不算——超预算才
        // 是信号）。默认 200ms ≈ 2 × 默认 tick 间隔（100ms）：超出两倍
        // 预算的 tick 视为诊断信号（05 §6 MVP 口径）。
        std::chrono::milliseconds slowThreshold{200};
    };

    TickDiagnostics();
    explicit TickDiagnostics(Config config);

    void onTickCompleted(std::uint64_t tickIndex, Duration duration) override;

    // 只读诊断视图：累计慢 tick 数；最近一次慢 tick 的序号仅在
    // slowTickCount() > 0 时有意义（tick 序号自 0 起，0 值即"无记录"）。
    std::uint64_t slowTickCount() const;
    std::uint64_t lastSlowTickIndex() const;

private:
    Config config_;
    std::uint64_t slowTickCount_ = 0;
    std::uint64_t lastSlowTickIndex_ = 0;
};

}  // namespace theseed::runtime
