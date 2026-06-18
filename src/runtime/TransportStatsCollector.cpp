#include "theseed/runtime/TransportStatsCollector.h"

namespace theseed::runtime {

TransportStatsCollector::TransportStatsCollector()
    : messagesSentTotal_(foundation::MetricsRegistry::instance().counter(
          "transport_messages_sent_total",
          "cumulative messages sent across all transport peers")),
      messagesReceivedTotal_(foundation::MetricsRegistry::instance().counter(
          "transport_messages_received_total",
          "cumulative messages received across all transport peers")),
      bytesSentTotal_(foundation::MetricsRegistry::instance().counter(
          "transport_bytes_sent_total",
          "cumulative bytes sent across all transport peers")),
      bytesReceivedTotal_(foundation::MetricsRegistry::instance().counter(
          "transport_bytes_received_total",
          "cumulative bytes received across all transport peers")),
      backPressureEventsTotal_(foundation::MetricsRegistry::instance().counter(
          "transport_backpressure_events_total",
          "cumulative transport backpressure events")),
      outboundQueueDepth_(foundation::MetricsRegistry::instance().gauge(
          "queue_backlog",
          "current outbound queue depth across all transport peers")),
      inboundQueueDepth_(foundation::MetricsRegistry::instance().gauge(
          "inbound_queue_depth",
          "current inbound queue depth across all transport peers")),
      backPressureActive_(foundation::MetricsRegistry::instance().gauge(
          "transport_backpressure",
          "1 if any peer currently reports backpressure, else 0")) {}

void TransportStatsCollector::collect(const TransportStats& stats) {
    if (initialized_) {
        messagesSentTotal_.increment(
            deltaOrZero(stats.messagesSent, last_.messagesSent));
        messagesReceivedTotal_.increment(
            deltaOrZero(stats.messagesReceived, last_.messagesReceived));
        bytesSentTotal_.increment(
            deltaOrZero(stats.bytesSent, last_.bytesSent));
        bytesReceivedTotal_.increment(
            deltaOrZero(stats.bytesReceived, last_.bytesReceived));
        backPressureEventsTotal_.increment(
            deltaOrZero(stats.backPressureEvents, last_.backPressureEvents));
    } else {
        messagesSentTotal_.increment(stats.messagesSent);
        messagesReceivedTotal_.increment(stats.messagesReceived);
        bytesSentTotal_.increment(stats.bytesSent);
        bytesReceivedTotal_.increment(stats.bytesReceived);
        backPressureEventsTotal_.increment(stats.backPressureEvents);
    }
    last_ = stats;
    initialized_ = true;

    outboundQueueDepth_.set(static_cast<std::int64_t>(stats.outboundQueueDepth));
    inboundQueueDepth_.set(static_cast<std::int64_t>(stats.inboundQueueDepth));
    backPressureActive_.set(stats.backPressureEvents > 0 ? 1 : 0);
}

void TransportStatsCollector::reset() {
    last_ = TransportStats{};
    initialized_ = false;
}

}  // namespace theseed::runtime
