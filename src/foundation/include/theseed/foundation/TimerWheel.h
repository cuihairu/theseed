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
    // 已分配条目总数（活跃 + 已取消未回收）。诊断/测试用：cancel 后必须
    // 回池复用，不应随取消次数无界增长。
    std::size_t totalEntries() const noexcept { return entries_.size(); }
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
        // 审计修复：记录条目当前所在槽位，cancel 时可 O(slot) 摘除并回池，
        // 否则取消的条目永远滞留 entries_（长跑进程无界增长）。
        int slotLevel = 0;
        int slotIndex = 0;
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
