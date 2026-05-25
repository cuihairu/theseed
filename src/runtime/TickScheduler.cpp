#include "theseed/runtime/TickScheduler.h"

#include <algorithm>
#include <chrono>
#include <thread>

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
