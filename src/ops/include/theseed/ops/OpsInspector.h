#pragma once

#include "theseed/runtime/RuntimeTypes.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace theseed::ops {

struct ProcessInfo final {
    std::string role;           // "BaseApp" / "DBApp" / "LoginApp" / ...
    std::string version = "0.1.0";
    std::chrono::system_clock::time_point startTime = std::chrono::system_clock::now();
    runtime::ComponentId componentId = 0;
};

struct RuntimeInfo final {
    std::size_t entityCount = 0;
    std::size_t sessionCount = 0;
    std::vector<std::string> entityTypes;
    runtime::TransportStats transportStats{};
};

// Snapshot-style inspect aggregator. Read-only: MVP Phase B per
// docs/design/5-access-and-control-plane/04-ops-control-plane.md section 8.
class OpsInspector final {
public:
    using Provider = std::function<RuntimeInfo()>;

    OpsInspector(ProcessInfo info, Provider provider = {});

    const ProcessInfo& process() const;

    // Pulls a fresh RuntimeInfo via the provider (or returns a default).
    RuntimeInfo snapshot() const;

    // Renders Prometheus text from the global MetricsRegistry.
    std::string renderMetrics() const;
    // Health JSON: role, version, uptime_s, startup/liveness/readiness (true).
    std::string renderHealthJson() const;
    // Inspect JSON: process info + runtime snapshot.
    std::string renderInspectJson() const;
    // Entities JSON: same as inspect but focused on entity listing.
    std::string renderEntitiesJson() const;

private:
    ProcessInfo info_;
    Provider provider_;
};

}  // namespace theseed::ops
