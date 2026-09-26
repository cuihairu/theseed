#pragma once

#include "theseed/runtime/RuntimeTypes.h"
#include "theseed/runtime/TickScheduler.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace theseed::runtime {

// 诊断采样能力接缝（04-ops-control-plane §7 的"按需触发一次采样" +
// "按 handle 查询/下载产物"）：控制面 daemon 只依赖本接口——采样对象
// 与产物存储的实现在 TickProfiler。句柄自 1 起单调递增；0 是
// "未产出/触发失败"哨兵值。
class ITickProfiler {
public:
    virtual ~ITickProfiler() = default;  // LCOV_EXCL_LINE C++ ABI：trivial 虚析构是空体，gcc 不为其生成计数指令，D0/D1/D2 三符号变体恒 0（结构不可测）

    // 产物元数据（下载走 payload，清单只暴露元数据）。
    struct ArtifactMeta final {
        std::uint64_t handle = 0;
        std::uint64_t tickCount = 0;  // 窗口内实际采到的 tick 数
        double windowMs = 0.0;        // 触发到窗口收满的实测墙钟跨度
    };

    // 触发一次固定窗口采样，返回窗口句柄；0 = 拒绝触发（已有窗口
    // 进行中——限流口径：同一时刻只允许一个窗口；或窗口配置无效）。
    virtual std::uint64_t trigger() = 0;

    // 是否有窗口进行中（触发前查重、测试观测用）。
    virtual bool sampling() const = 0;

    // 已完成产物清单（时间升序 = 完成序；环形满后旧产物被逐出）。
    virtual std::vector<ArtifactMeta> listArtifacts() const = 0;

    // 按 handle 取产物字节（JSON 快照，只读）；未知句柄返回 false。
    virtual bool artifactPayload(std::uint64_t handle, std::string& out) const = 0;
};

// tick 粒度采样器（05 §6 MVP：不做 flamegraph 全量采样、不做
// EntityProfiler→负载反馈链——实体级信号归 EntityLoadProfiler）。
//
// 工作方式：作为 ITickObserver 挂到 TickScheduler（setObserver），
// trigger() 开一个固定 tick 数的窗口，窗口内逐 tick 记录实测耗时，
// 收满即固化产物（JSON 快照：样本 + min/max/avg + 超阈值样本数）。
// 窗口进行中重复触发被拒（限流）；产物进环形存储（满后丢最旧）。
//
// 线程纪律：trigger/查询与 onTickCompleted 同线程调用（调度器 tick
// 线程或其宿主），内部无锁。
class TickProfiler final : public ITickProfiler, public ITickObserver {
public:
    struct Config final {
        // 固定窗口的 tick 数（05 §6"按需触发一次采样"的 MVP 口径：
        // 固定窗口快照，而非持续 profiling）。0 = 配置无效，触发恒拒。
        std::uint64_t windowTicks = 32;
        // 产物环形容量：满后丢最旧（与审计环形同一容量纪律）；0 视为 1。
        std::size_t maxArtifacts = 8;
        // 快照统计用的慢 tick 阈值（毫秒，严格大于算慢）：只进产物
        // 报告字段 slow_samples，不另发告警——全局慢告警归
        // TickDiagnostics 的 slow_tick_count，两处口径分开。
        std::chrono::milliseconds slowThreshold{200};
    };

    TickProfiler();
    explicit TickProfiler(Config config);

    // ITickProfiler
    std::uint64_t trigger() override;
    bool sampling() const override;
    std::vector<ArtifactMeta> listArtifacts() const override;
    bool artifactPayload(std::uint64_t handle, std::string& out) const override;

    // ITickObserver：窗口进行中逐 tick 记样本，收满即固化产物；
    // 空闲时的 tick 静默忽略。
    void onTickCompleted(std::uint64_t tickIndex, Duration duration) override;

private:
    struct Sampled {
        std::uint64_t handle = 0;
        std::uint64_t tickCount = 0;
        double windowMs = 0.0;
        std::string payload;
    };

    void finalizeWindow();

    Config config_;
    bool sampling_ = false;
    std::uint64_t nextHandle_ = 0;
    std::uint64_t openHandle_ = 0;
    std::vector<double> samples_;
    std::chrono::steady_clock::time_point windowStart_{};
    std::vector<Sampled> artifacts_;
};

}  // namespace theseed::runtime
