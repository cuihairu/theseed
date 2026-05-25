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

}  // namespace theseed::runtime
