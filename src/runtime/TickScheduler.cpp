#include "theseed/runtime/TickScheduler.h"
#include "theseed/foundation/Metrics.h"
#include "theseed/foundation/Tracing.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>

namespace theseed::runtime {

namespace {

constexpr std::size_t toIndex(TickPhase phase) {
    return static_cast<std::size_t>(phase);
}

}  // namespace

TickScheduler::TickScheduler(Duration tickInterval) : tickInterval_(tickInterval) {}

void TickScheduler::registerTickable(TickPhase phase, ITickable& tickable) {
    std::lock_guard lock(mutex_);
    auto& phaseTickables = tickables_[toIndex(phase)];
    if (std::find(phaseTickables.begin(), phaseTickables.end(), &tickable) ==
        phaseTickables.end()) {
        phaseTickables.push_back(&tickable);
    }
}

bool TickScheduler::unregisterTickable(TickPhase phase, ITickable& tickable) {
    std::lock_guard lock(mutex_);
    auto& phaseTickables = tickables_[toIndex(phase)];
    const auto iter =
        std::find(phaseTickables.begin(), phaseTickables.end(), &tickable);
    if (iter == phaseTickables.end()) {
        return false;
    }

    phaseTickables.erase(iter);
    return true;
}

void TickScheduler::post(Task task) {
    if (!task) {
        return;
    }

    std::lock_guard lock(mutex_);
    pendingTasks_.push_back(std::move(task));
}

void TickScheduler::requestStop() {
    stopRequested_.store(true, std::memory_order_release);
}

bool TickScheduler::running() const {
    return running_.load(std::memory_order_acquire);
}

std::uint64_t TickScheduler::currentTick() const {
    std::lock_guard lock(mutex_);
    return currentTick_;
}

Duration TickScheduler::tickInterval() const {
    return tickInterval_;
}

Duration TickScheduler::lastTickDuration() const {
    std::lock_guard lock(mutex_);
    return lastTickDuration_;
}

std::vector<ITickable*> TickScheduler::snapshot(TickPhase phase) const {
    std::lock_guard lock(mutex_);
    return tickables_[toIndex(phase)];
}

void TickScheduler::executeTasks(std::vector<Task>& tasks) const {
    for (auto& task : tasks) {
        task();
    }
}

void TickScheduler::executePhase(TickPhase phase, TickContext& context) {
    const auto tickables = snapshot(phase);
    for (auto* tickable : tickables) {
        tickable->tick(context);
        if (context.shouldStop) {
            break;
        }
    }
}

void TickScheduler::runOnce() {
    if (stopRequested_.load(std::memory_order_acquire)) {
        return;
    }

    std::vector<Task> tasks;
    {
        std::lock_guard lock(mutex_);
        tasks.swap(pendingTasks_);
    }

    // 关键链路 trace：整个 tick 作为一个 span。默认 emitter 是 no-op，
    // 但 currentSpanContext 会被 Logger 注入到 tick 内的所有日志，
    // 实现 log-trace 关联（MVP §12「关键链路 Trace」）。
    auto tickSpan = theseed::foundation::startSpan("tick");

    const auto start = Clock::now();
    TickContext context;
    {
        std::lock_guard lock(mutex_);
        context.tickIndex = currentTick_;
        context.deltaTime = tickInterval_;
        context.budget = tickInterval_;
    }

    executeTasks(tasks);

    executePhase(TickPhase::Network, context);
    executePhase(TickPhase::Timer, context);
    executePhase(TickPhase::Entity, context);
    executePhase(TickPhase::Script, context);
    executePhase(TickPhase::SyncBuild, context);
    executePhase(TickPhase::Flush, context);

    if (context.shouldStop) {
        requestStop();
    }

    const auto elapsed = Clock::now() - start;
    {
        std::lock_guard lock(mutex_);
        lastTickDuration_ = elapsed;
        context.elapsed = elapsed;
        ++currentTick_;
    }

    // span 收尾：记录耗时，便于 emitter 导出。
    tickSpan.setAttribute("tick_duration_ms",
                          std::chrono::duration<double, std::milli>(elapsed).count());

    // Phase B MVP metric: tick duration distribution (ms).
    auto& tickMetric = theseed::foundation::MetricsRegistry::instance().histogram(
        "tick_duration_ms",
        theseed::foundation::Histogram::Boundaries{1.0, 2.0, 5.0, 10.0, 25.0, 50.0,
                                                    100.0, 250.0, 500.0, 1000.0},
        "tick wall-clock duration in milliseconds");
    tickMetric.observe(
        std::chrono::duration<double, std::milli>(elapsed).count());
}

void TickScheduler::run() {
    running_.store(true, std::memory_order_release);
    stopRequested_.store(false, std::memory_order_release);

    auto nextWake = Clock::now();
    while (!stopRequested_.load(std::memory_order_acquire)) {
        runOnce();
        if (stopRequested_.load(std::memory_order_acquire)) {
            break;
        }

        if (tickInterval_ > Duration::zero()) {
            nextWake += tickInterval_;
            std::this_thread::sleep_until(nextWake);
        } else {
            std::this_thread::yield();
        }
    }

    running_.store(false, std::memory_order_release);
}

}  // namespace theseed::runtime