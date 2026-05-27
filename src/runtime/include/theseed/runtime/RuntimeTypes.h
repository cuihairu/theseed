#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>

namespace theseed::runtime {

using EntityId = std::uint64_t;
using ComponentId = std::uint32_t;
using MethodId = std::uint32_t;
using MigrationEpoch = std::uint64_t;
using MigrationToken = std::uint64_t;
using DetailLevel = std::uint8_t;
using SpaceId = std::uint64_t;
using CellId = std::uint32_t;

enum class DeliveryClass : std::uint8_t {
    ORDERED_RELIABLE = 0,
    UNORDERED_LOSSY = 1,
};

enum class ChannelClass : std::uint8_t {
    Reliable = 0,
    Unreliable = 1,
    Control = 2,
};

struct ChannelKey final {
    ComponentId peer = 0;
    ChannelClass class_ = ChannelClass::Reliable;

    friend bool operator==(const ChannelKey&, const ChannelKey&) = default;
};

struct ChannelKeyHash final {
    std::size_t operator()(const ChannelKey& key) const noexcept {
        auto h = static_cast<std::size_t>(key.peer);
        h ^= static_cast<std::size_t>(key.class_) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

struct Vector3 final {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;

    friend bool operator==(const Vector3&, const Vector3&) = default;
};

inline float distanceSquared(const Vector3& lhs, const Vector3& rhs) {
    const auto dx = lhs.x - rhs.x;
    const auto dy = lhs.y - rhs.y;
    const auto dz = lhs.z - rhs.z;
    return dx * dx + dy * dy + dz * dz;
}

using Clock = std::chrono::steady_clock;
using Duration = Clock::duration;
using TimePoint = Clock::time_point;

enum class SendResult : std::uint8_t {
    Accepted = 0,
    BackPressure,
    NotConnected,
    Closed,
    Oversized,
};

struct TransportStats final {
    std::size_t messagesSent = 0;
    std::size_t messagesReceived = 0;
    std::size_t bytesSent = 0;
    std::size_t bytesReceived = 0;
    std::size_t outboundQueueDepth = 0;
    std::size_t inboundQueueDepth = 0;
    std::size_t backPressureEvents = 0;
};

}  // namespace theseed::runtime
