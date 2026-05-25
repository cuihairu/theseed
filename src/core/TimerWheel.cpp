#include "theseed/core/TimerWheel.h"

#include <algorithm>
#include <utility>

namespace theseed::core {

TimerWheel::TimerWheel(Duration tickDuration)
    : tickDuration_(tickDuration) {}

std::size_t TimerWheel::allocEntry() {
    if (!freeList_.empty()) {
        auto index = freeList_.back();
        freeList_.pop_back();
        return index;
    }
    auto index = entries_.size();
    entries_.emplace_back();
    return index;
}

void TimerWheel::insertEntry(std::size_t index) {
    const auto fireTick = entries_[index].fireTick;
    const auto diff = fireTick > currentTick_ ? fireTick - currentTick_ : 0;

    // Determine the appropriate level for this timer
    static constexpr std::uint64_t kLevelMask[kLevels] = {
        0xFFULL,
        0xFFFFULL,
        0xFFFFFFULL,
        0xFFFFFFFFULL,
    };

    int level = 0;
    while (level + 1 < kLevels && diff > kLevelMask[level]) {
        ++level;
    }

    auto slotIndex = static_cast<int>((fireTick >> (8 * level)) & 0xFF);
    wheels_[level][slotIndex].push_back(index);
}

void TimerWheel::fireEntry(std::size_t index) {
    auto& entry = entries_[index];

    if (entry.period > 0) {
        // Periodic: reschedule then fire (keep callback alive)
        entry.fireTick = currentTick_ + entry.period;
        insertEntry(index);
        if (entry.callback) entry.callback();
    } else {
        // One-shot: extract callback, mark done, then fire
        auto cb = std::move(entry.callback);
        entry.callback = nullptr;
        entry.cancelled = true;
        freeList_.push_back(index);
        if (cb) cb();
    }
}

TimerHandle TimerWheel::addTimer(Duration delay, Callback callback) {
    const auto ticks = static_cast<std::uint64_t>(
        std::max<std::int64_t>(1, delay / tickDuration_));

    auto index = allocEntry();
    entries_[index] = {nextTimerId_, 0, currentTick_ + ticks, 0, std::move(callback), false};
    insertEntry(index);

    TimerHandle handle{nextTimerId_, 0};
    ++nextTimerId_;
    return handle;
}

TimerHandle TimerWheel::addPeriodic(Duration interval, Callback callback) {
    const auto ticks = static_cast<std::uint64_t>(
        std::max<std::int64_t>(1, interval / tickDuration_));

    auto index = allocEntry();
    entries_[index] = {nextTimerId_, 0, currentTick_ + ticks, ticks, std::move(callback), false};
    insertEntry(index);

    TimerHandle handle{nextTimerId_, 0};
    ++nextTimerId_;
    return handle;
}

bool TimerWheel::cancel(TimerHandle handle) {
    for (auto& entry : entries_) {
        if (entry.id == handle.id && entry.generation == handle.generation && !entry.cancelled) {
            entry.cancelled = true;
            return true;
        }
    }
    return false;
}

void TimerWheel::advance(Duration dt) {
    const auto ticksToAdvance = static_cast<std::uint64_t>(
        std::max<std::int64_t>(1, dt / tickDuration_));

    for (std::uint64_t t = 0; t < ticksToAdvance; ++t) {
        ++currentTick_;

        // 1. Fire all entries in level 0 current slot
        auto& slot0 = wheels_[0][currentTick_ & 0xFF];
        auto entries0 = std::move(slot0);
        slot0.clear();

        for (auto idx : entries0) {
            if (!entries_[idx].cancelled) {
                fireEntry(idx);
            }
        }

        // 2. Cascade from higher levels when lower levels complete a cycle
        for (int level = 1; level < kLevels; ++level) {
            const auto mask = (1ULL << (8 * level)) - 1;
            if ((currentTick_ & mask) != 0) break;

            auto slotIdx = static_cast<int>((currentTick_ >> (8 * level)) & 0xFF);
            auto& slot = wheels_[level][slotIdx];
            auto moved = std::move(slot);
            slot.clear();

            for (auto idx : moved) {
                if (!entries_[idx].cancelled) {
                    insertEntry(idx);
                }
            }
        }
    }
}

std::size_t TimerWheel::activeCount() const {
    std::size_t count = 0;
    for (const auto& entry : entries_) {
        if (!entry.cancelled) ++count;
    }
    return count;
}

void TimerWheel::clear() {
    entries_.clear();
    freeList_.clear();
    for (auto& level : wheels_) {
        for (auto& slot : level) {
            slot.clear();
        }
    }
}

}  // namespace theseed::core
