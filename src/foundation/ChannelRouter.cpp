#include "theseed/foundation/ChannelRouter.h"

#include <algorithm>

namespace theseed::foundation {

Channel& ChannelRouter::getOrCreate(RoutingKey key) {
    auto it = channels_.find(key);
    if (it != channels_.end()) {
        return *it->second;
    }

    auto channel = std::make_unique<Channel>(key.peer);
    channel->setWatermark(defaultWatermark_);
    auto& ref = *channel;
    channels_.emplace(key, std::move(channel));
    return ref;
}

Channel* ChannelRouter::find(RoutingKey key) {
    auto it = channels_.find(key);
    return it != channels_.end() ? it->second.get() : nullptr;
}

bool ChannelRouter::closeChannel(RoutingKey key) {
    return channels_.erase(key) > 0;
}

std::vector<ChannelRouter::RoutingKey> ChannelRouter::sortedKeys() const {
    std::vector<RoutingKey> keys;
    keys.reserve(channels_.size());
    for (const auto& [key, _] : channels_) {
        keys.push_back(key);
    }
    // Priority: Control(2) > Reliable(0) > Unreliable(1)
    std::sort(keys.begin(), keys.end(), [](const RoutingKey& a, const RoutingKey& b) {
        auto priority = [](std::uint8_t cls) -> int {
            if (cls == 2) return 0;  // Control - highest
            if (cls == 0) return 1;  // Reliable
            return 2;                // Unreliable - lowest
        };
        return priority(a.class_) < priority(b.class_);
    });
    return keys;
}

std::size_t ChannelRouter::drainAll(MemoryStream& out) {
    std::size_t totalDrained = 0;
    for (const auto& key : sortedKeys()) {
        auto it = channels_.find(key);
        if (it != channels_.end() && it->second->hasPending()) {
            auto before = out.size();
            it->second->drain(out);
            totalDrained += out.size() - before;
        }
    }
    return totalDrained;
}

std::size_t ChannelRouter::totalPendingCount() const {
    std::size_t total = 0;
    for (const auto& [_, channel] : channels_) {
        total += channel->pendingBundleCount();
    }
    return total;
}

bool ChannelRouter::hasBackPressuredChannel() const {
    for (const auto& [_, channel] : channels_) {
        if (channel->isBackPressured()) {
            return true;
        }
    }
    return false;
}

std::size_t ChannelRouter::channelCount() const {
    return channels_.size();
}

void ChannelRouter::setDefaultWatermark(Channel::Watermark wm) {
    defaultWatermark_ = wm;
}

void ChannelRouter::setWatermark(RoutingKey key, Channel::Watermark wm) {
    auto* ch = find(key);
    if (ch) {
        ch->setWatermark(wm);
    }
}

void ChannelRouter::setOverflowPolicy(RoutingKey key, Channel::OverflowPolicy policy) {
    auto* ch = find(key);
    if (ch) {
        ch->setOverflowPolicy(policy);
    }
}

}  // namespace theseed::foundation
