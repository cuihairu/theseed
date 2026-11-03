#include "theseed/foundation/TimerWheel.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace theseed::foundation {

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
    // 记录条目当前槽位：cancel 时据此 O(slot) 摘除，并立即回池复用。
    entries_[index].slotLevel = level;
    entries_[index].slotIndex = slotIndex;
}

void TimerWheel::fireEntry(std::size_t index) {
    auto& entry = entries_[index];

    if (entry.period > 0) {
        entry.fireTick = currentTick_ + entry.period;
        insertEntry(index);
        if (entry.callback) entry.callback();
    } else {
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
}  // LCOV_EXCL_BR_LINE 函数尾汇合伪边归因本行：std::max 两臂已由 sub-tick/正常 delay 场景覆盖，无残余业务分支

TimerHandle TimerWheel::addPeriodic(Duration interval, Callback callback) {
    const auto ticks = static_cast<std::uint64_t>(
        std::max<std::int64_t>(1, interval / tickDuration_));

    auto index = allocEntry();
    entries_[index] = {nextTimerId_, 0, currentTick_ + ticks, ticks, std::move(callback), false};
    insertEntry(index);

    TimerHandle handle{nextTimerId_, 0};
    ++nextTimerId_;
    return handle;
}  // LCOV_EXCL_BR_LINE 同 addTimer：函数尾汇合伪边归因本行，两臂均已覆盖

bool TimerWheel::cancel(TimerHandle handle) {
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        auto& entry = entries_[index];
        if (entry.id == handle.id && entry.generation == handle.generation &&
            !entry.cancelled) {
            // 从所在槽位摘除并立即回池：否则取消条目永远滞留 entries_，
            // 长跑进程取消无数定时器会无界增长（P2 深审）。摘除后 advance
            // 不再见到该索引，不会误触发复用后的新条目。
            auto& slot = wheels_[entry.slotLevel][entry.slotIndex];
            slot.erase(std::remove(slot.begin(), slot.end(), index), slot.end());
            entry.cancelled = true;
            freeList_.push_back(index);
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

        auto& slot0 = wheels_[0][currentTick_ & 0xFF];
        // 快照必须携带条目 id：批内回调可能 cancel 某索引（回池）并 addTimer
        // 复用该索引，此时快照里的索引已指向全新条目；仅按索引重触发会把
        // 新定时器提前触发并重复插入轮盘。按 (index,id) 双重校验，复用槽
        // 的 id 不匹配即跳过（审核发现）。
        std::vector<std::pair<std::size_t, std::uint64_t>> entries0;
        entries0.reserve(slot0.size());
        for (const auto idx : slot0) {
            entries0.emplace_back(idx, entries_[idx].id);
        }
        slot0.clear();

        for (const auto& [idx, id] : entries0) {
            if (entries_[idx].id == id && !entries_[idx].cancelled) {
                fireEntry(idx);
            }
        }

        for (int level = 1; level < kLevels; ++level) {
            const auto mask = (1ULL << (8 * level)) - 1;
            if ((currentTick_ & mask) != 0) break;

            auto slotIdx = static_cast<int>((currentTick_ >> (8 * level)) & 0xFF);
            auto& slot = wheels_[level][slotIdx];
            // 同 level-0：快照携带 id，防止批内复用槽被再次处理。
            std::vector<std::pair<std::size_t, std::uint64_t>> moved;
            moved.reserve(slot.size());
            for (const auto idx : slot) {
                moved.emplace_back(idx, entries_[idx].id);
            }
            slot.clear();

            for (const auto& [idx, id] : moved) {
                if (entries_[idx].id == id && !entries_[idx].cancelled) {
                    // 级联时 fireTick 恰为 currentTick_（周期为 256 的倍数）的
                    // 条目 diff=0，insertEntry 会把它插回本 tick 已排空的
                    // level-0 槽，导致晚一轮（256 tick）才触发、周期翻倍。
                    // 到期条目必须立即触发，不得重插。
                    if (entries_[idx].fireTick <= currentTick_) {
                        fireEntry(idx);
                    } else {
                        insertEntry(idx);
                    }
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

}  // namespace theseed::foundation
