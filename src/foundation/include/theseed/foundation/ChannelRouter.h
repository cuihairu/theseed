#pragma once

#include "theseed/foundation/Channel.h"

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

namespace theseed::foundation {

class ChannelRouter final {
public:
    ChannelRouter() = default;

    ChannelRouter(const ChannelRouter&) = delete;
    ChannelRouter& operator=(const ChannelRouter&) = delete;

    struct RoutingKey final {
        std::uint32_t peer = 0;
        std::uint8_t class_ = 0;

        friend bool operator==(const RoutingKey&, const RoutingKey&) = default;
    };

    struct RoutingKeyHash final {
        std::size_t operator()(const RoutingKey& key) const noexcept {
            auto h = static_cast<std::size_t>(key.peer);
            h ^= static_cast<std::size_t>(key.class_) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    Channel& getOrCreate(RoutingKey key);
    Channel* find(RoutingKey key);
    bool closeChannel(RoutingKey key);

    // Drain all channels in priority order: Control(2) > Reliable(0) > Unreliable(1)
    std::size_t drainAll(MemoryStream& out);

    std::size_t totalPendingCount() const;
    bool hasBackPressuredChannel() const;
    std::size_t channelCount() const;

    void setDefaultWatermark(Channel::Watermark wm);
    void setWatermark(RoutingKey key, Channel::Watermark wm);
    void setOverflowPolicy(RoutingKey key, Channel::OverflowPolicy policy);

private:
    std::vector<RoutingKey> sortedKeys() const;

    std::unordered_map<RoutingKey, std::unique_ptr<Channel>, RoutingKeyHash> channels_;
    Channel::Watermark defaultWatermark_;
};

}  // namespace theseed::foundation
