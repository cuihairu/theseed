#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace theseed::foundation {

struct TimerHandle final {
    std::uint64_t id = 0;
    std::uint32_t generation = 0;

    explicit operator bool() const { return id != 0; }
    friend bool operator==(const TimerHandle&, const TimerHandle&) = default;
};

class TimerWheel final {
public:
    using Callback = std::function<void()>;
    using Duration = std::chrono::steady_clock::duration;

    explicit TimerWheel(Duration tickDuration = std::chrono::milliseconds{10});

    TimerWheel(const TimerWheel&) = delete;
    TimerWheel& operator=(const TimerWheel&) = delete;

    TimerHandle addTimer(Duration delay, Callback callback);
    TimerHandle addPeriodic(Duration interval, Callback callback);
    bool cancel(TimerHandle handle);

    void advance(Duration dt);

    std::size_t activeCount() const;
    void clear();

private:
    static constexpr int kLevels = 4;
    static constexpr int kSlotsPerLevel = 256;

    struct TimerEntry {
        std::uint64_t id = 0;
        std::uint32_t generation = 0;
        std::uint64_t fireTick = 0;
        std::uint64_t period = 0;
        Callback callback;
        bool cancelled = true;
    };

    std::size_t allocEntry();
    void insertEntry(std::size_t index);
    void fireEntry(std::size_t index);

    Duration tickDuration_;
    std::uint64_t currentTick_ = 0;
    std::uint64_t nextTimerId_ = 1;

    std::vector<TimerEntry> entries_;
    std::vector<std::size_t> freeList_;

    std::array<std::array<std::vector<std::size_t>, kSlotsPerLevel>, kLevels> wheels_;
};

}  // namespace theseed::foundation
