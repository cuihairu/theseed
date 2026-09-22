#pragma once

#include "theseed/runtime/RuntimeTypes.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace theseed::runtime {

// MVP Phase C-2: Entity / EntityType load profiler.
//
// Source model: BigWorld's EntityProfiler / EntityTypeProfiler produce runtime
// load signals that feed back into scheduling (loadBalance / shouldOffload /
// overloadCheck). This prototype provides the signal-generation half — EMA
// smoothing, artificial min-load overrides — without yet wiring the output
// into Cell/Space balancers (those land in a later phase).
//
// All loads are expressed in milliseconds per tick.

struct EntityLoadSnapshot final {
    EntityId entityId = 0;
    std::string entityType;
    float rawLoad = 0.0F;          // accumulated ms this tick
    float smoothedLoad = 0.0F;     // exponential moving average
    float adjustedLoad = 0.0F;     // max(smoothed, artificialMin)
    float artificialMinLoad = 0.0F;
};

struct EntityTypeLoadSnapshot final {
    std::string entityTypeId;
    std::uint32_t entityCount = 0;
    float totalRawLoad = 0.0F;
    float totalSmoothedLoad = 0.0F;
    float maxRawLoad = 0.0F;
    float maxSmoothedLoad = 0.0F;
};

class EntityLoadProfiler final {
public:
    struct Config final {
        float emaAlpha = 0.3F;
    };

    EntityLoadProfiler();
    explicit EntityLoadProfiler(Config config);

    // RAII scope measuring wall-clock cost spent inside a per-entity work block.
    class Scope final {
    public:
        Scope(EntityLoadProfiler& profiler, EntityId id, std::string type);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        EntityLoadProfiler& profiler_;
        EntityId id_;
        std::string type_;
        std::chrono::steady_clock::time_point start_;
    };

    [[nodiscard]] Scope scope(EntityId id, std::string_view type);

    // Advance EMA for all tracked entities by one tick and emit fresh raw samples.
    // Should be invoked once per tick from the host loop.
    void tick();

    EntityLoadSnapshot snapshot(EntityId id) const;
    void setArtificialMinLoad(EntityId id, float load);

    std::vector<EntityLoadSnapshot> all() const;
    std::size_t trackedCount() const noexcept;
    void reset();

    float emaAlpha() const noexcept;

private:
    struct Entry final {
        std::string entityType;
        float artificialMinLoad = 0.0F;
        float currentRawLoad = 0.0F;   // ms accumulated this tick
        float lastRawLoad = 0.0F;      // last completed tick
        float smoothedLoad = 0.0F;
    };

    void accumulate(EntityId id, std::string_view type, double ms);

    Config config_;
    std::unordered_map<EntityId, Entry> entries_;
};

// Aggregates per-entity snapshots into per-type load profiles.
class EntityTypeLoadAggregator final {
public:
    void record(const EntityLoadSnapshot& snapshot);
    EntityTypeLoadSnapshot snapshot(const std::string& typeId) const;
    std::vector<EntityTypeLoadSnapshot> all() const;
    void reset();

private:
    std::unordered_map<std::string, EntityTypeLoadSnapshot> perType_;
};

}  // namespace theseed::runtime
