#pragma once

#include "theseed/runtime/RuntimeTypes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace theseed::runtime {

// MVP Phase C-4 prototype: BackupTopologyCoordinator.
//
// Models the cluster-level backup ownership chain described in
// docs/design/3-cluster-and-availability/02-backup-hash-and-ha.md:
//   EntityId → (primary, backup) process mapping,
// with explicit prepare → prime → ack → promote transition so future HA
// work can hang off the same shape. This prototype uses deterministic
// modulo hashing for placement (KISS); a real implementation will swap in
// consistent hashing without changing the public surface.

struct BackupRoute final {
    EntityId entityId = 0;
    ComponentId primary = 0;
    ComponentId backup = 0;
    std::uint64_t epoch = 0;
};

struct BackupTopologyVersion final {
    std::uint64_t version = 0;
    std::uint64_t epoch = 0;

    bool operator==(const BackupTopologyVersion& other) const noexcept {
        return version == other.version && epoch == other.epoch;
    }
};

enum class TopologyState : std::uint8_t {
    Stable = 0,
    Rebuilding,
    Priming,
    Promoting,
    Aborted,
};

class BackupTopologyCoordinator final {
public:
    BackupTopologyCoordinator() = default;

    // Process membership. registerProcess returns false on duplicate id.
    bool registerProcess(ComponentId id);
    bool unregisterProcess(ComponentId id);
    std::size_t processCount() const noexcept;
    std::vector<ComponentId> processes() const;
    bool hasProcess(ComponentId id) const;

    // Active placement lookup. Returns nullopt if no processes are registered
    // or fewer than 2 are available (cannot satisfy backup requirement).
    std::optional<BackupRoute> routeFor(EntityId entityId) const;

    BackupTopologyVersion activeVersion() const noexcept;
    std::optional<BackupTopologyVersion> stagingVersion() const noexcept;
    TopologyState state() const noexcept;

    // Lifecycle transitions.
    // beginRebuild: bumps topology version, marks state=Priming, clears acks.
    //   Returns false if a rebuild is already in progress.
    bool beginRebuild(ComponentId reasonProcess, std::uint64_t newEpoch);

    // ackPrimed: a process reports it has primed its share of the new chain.
    // Returns false if the ack is for an unexpected version or unknown process.
    bool ackPrimed(ComponentId processId, BackupTopologyVersion version);

    // promote: caller invokes this once all acks are in. Returns true on success.
    bool promote();

    // abort: cancel an in-progress rebuild, returns to Stable.
    bool abort();

    // Returns true if all registered processes have acked the staging version.
    bool allAcked() const;

    // Test helper.
    void reset();

private:
    struct ProcessEntry final {
        ComponentId id = 0;
        bool acked = false;
    };

    std::vector<ProcessEntry> processes_;
    std::unordered_map<ComponentId, std::size_t> processIndex_;
    BackupTopologyVersion activeVersion_{};
    BackupTopologyVersion stagingVersion_{};
    bool hasStaging_ = false;
    TopologyState state_ = TopologyState::Stable;
    ComponentId rebuildReason_ = 0;
};

}  // namespace theseed::runtime
