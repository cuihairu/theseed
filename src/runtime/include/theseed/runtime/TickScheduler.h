#pragma once

#include "theseed/runtime/RuntimeTypes.h"

#include <array>
#include <atomic>
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
    virtual ~ITickable() = default;

    virtual void tick(TickContext& context) = 0;
};

class TickScheduler final {
public:
    using Task = std::function<void()>;

    explicit TickScheduler(Duration tickInterval = std::chrono::milliseconds{100});

    TickScheduler(const TickScheduler&) = delete;
    TickScheduler& operator=(const TickScheduler&) = delete;

    void registerTickable(TickPhase phase, ITickable& tickable);
    bool unregisterTickable(TickPhase phase, ITickable& tickable);

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
