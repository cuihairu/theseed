#pragma once

#include "theseed/runtime/RuntimeTypes.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace theseed::runtime {

enum class TickPhase : std::uint8_t {
    Network = 0,
    Timer,
    Entity,
    Script,
    SyncBuild,
    Flush,
    Count,
};

struct TickContext {
    std::uint64_t tickIndex = 0;
    Duration deltaTime{};
    Duration budget{};
    Duration elapsed{};
    bool shouldStop = false;
};

class ITickable {
public:
    virtual ~ITickable() = default;  // LCOV_EXCL_LINE C++ ABI：trivial 虚析构是空体，gcc 不为其生成计数指令，D0/D1/D2 三符号变体恒 0（结构不可测）

    virtual void tick(TickContext& context) = 0;
};

// tick 完成观察者（05-telemetry §6 慢 tick 诊断的只读接缝）：调度器只
// 广播事实（tick 序号 + 实测耗时），阈值分类与告警归诊断组件——策略
// 不进调度器。观察者不持有（调用方保证生命周期覆盖调度器）；回调在
// tick 线程内联执行，实现不得阻塞。
class ITickObserver {
public:
    virtual ~ITickObserver() = default;  // LCOV_EXCL_LINE C++ ABI：trivial 虚析构是空体，gcc 不为其生成计数指令，D0/D1/D2 三符号变体恒 0（结构不可测）

    virtual void onTickCompleted(std::uint64_t tickIndex, Duration duration) = 0;
};

class TickScheduler final {
public:
    using Task = std::function<void()>;

    explicit TickScheduler(Duration tickInterval = std::chrono::milliseconds{100});

    TickScheduler(const TickScheduler&) = delete;
    TickScheduler& operator=(const TickScheduler&) = delete;

    void registerTickable(TickPhase phase, ITickable& tickable);
    bool unregisterTickable(TickPhase phase, ITickable& tickable);

    // 挂接 tick 完成观察者（nullptr = 关闭诊断广播）。不持有；建议在
    // 调度线程启动前设置，运行期换绑需调用方与 tick 线程自行同步。
    void setObserver(ITickObserver* observer);
    ITickObserver* observer() const;

    void post(Task task);
    void requestStop();

    bool running() const;
    std::uint64_t currentTick() const;
    Duration tickInterval() const;
    Duration lastTickDuration() const;

    void runOnce();
    void run();

private:
    static constexpr std::size_t phaseCount = static_cast<std::size_t>(TickPhase::Count);

    std::vector<ITickable*> snapshot(TickPhase phase) const;
    void executeTasks(std::vector<Task>& tasks) const;
    void executePhase(TickPhase phase, TickContext& context);

    ITickObserver* observer_ = nullptr;

    Duration tickInterval_;
    mutable std::mutex mutex_;
    std::array<std::vector<ITickable*>, phaseCount> tickables_{};
    std::vector<Task> pendingTasks_;
    std::uint64_t currentTick_ = 0;
    Duration lastTickDuration_{};
    std::atomic_bool running_{false};
    std::atomic_bool stopRequested_{false};
};

}  // namespace theseed::runtime
