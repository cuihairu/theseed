#pragma once

#include "theseed/foundation/Metrics.h"
#include "theseed/runtime/RuntimeTypes.h"

namespace theseed::runtime {

// Aggregates TransportStats into the global MetricsRegistry.
//
// Counter metrics use delta mode: the registry holds monotonically increasing
// totals while each tick reports the cumulative counter maintained by the
// transport, so the collector computes per-tick deltas and increments the
// registry counter by that delta.
//
// Gauge metrics are set to the latest sampled value.
//
// A collector instance is meant to be owned by a single app/process and called
// once per tick.
class TransportStatsCollector final {
public:
    TransportStatsCollector();

    void collect(const TransportStats& stats);

    // Test-only: clears the "previous snapshot" so the next collect() emits
    // raw totals as deltas (i.e. mirrors freshly-constructed state).
    void reset();

private:
    static std::size_t deltaOrZero(std::size_t current, std::size_t previous) {
        return current >= previous ? current - previous : 0;
    }

    TransportStats last_;
    bool initialized_ = false;

    foundation::Counter& messagesSentTotal_;
    foundation::Counter& messagesReceivedTotal_;
    foundation::Counter& bytesSentTotal_;
    foundation::Counter& bytesReceivedTotal_;
    foundation::Counter& backPressureEventsTotal_;
    foundation::Gauge& outboundQueueDepth_;
    foundation::Gauge& inboundQueueDepth_;
    foundation::Gauge& backPressureActive_;
};

}  // namespace theseed::runtime
