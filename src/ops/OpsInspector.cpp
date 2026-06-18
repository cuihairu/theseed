#include "theseed/ops/OpsInspector.h"

#include "theseed/foundation/Metrics.h"

#include <sstream>
#include <utility>

namespace theseed::ops {

namespace {

std::string escapeJson(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;
        }
    }
    return out;
}

std::int64_t uptimeSeconds(std::chrono::system_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now() - start).count();
}

}  // namespace

OpsInspector::OpsInspector(ProcessInfo info, Provider provider)
    : info_(std::move(info)), provider_(std::move(provider)) {}

const ProcessInfo& OpsInspector::process() const { return info_; }

RuntimeInfo OpsInspector::snapshot() const {
    if (provider_) return provider_();
    return RuntimeInfo{};
}

std::string OpsInspector::renderMetrics() const {
    return foundation::MetricsRegistry::instance().renderText();
}

std::string OpsInspector::renderHealthJson() const {
    const auto uptime = uptimeSeconds(info_.startTime);
    std::ostringstream out;
    out << '{';
    out << "\"role\":\"" << escapeJson(info_.role) << "\",";
    out << "\"version\":\"" << escapeJson(info_.version) << "\",";
    out << "\"component_id\":" << static_cast<std::uint64_t>(info_.componentId) << ',';
    out << "\"uptime_s\":" << uptime << ',';
    out << "\"startup\":true,";
    out << "\"liveness\":true,";
    out << "\"readiness\":true";
    out << '}';
    return out.str();
}

std::string OpsInspector::renderInspectJson() const {
    const auto runtime = snapshot();
    const auto uptime = uptimeSeconds(info_.startTime);

    std::ostringstream out;
    out << '{';
    out << "\"process\":{";
    out << "\"role\":\"" << escapeJson(info_.role) << "\",";
    out << "\"version\":\"" << escapeJson(info_.version) << "\",";
    out << "\"component_id\":" << static_cast<std::uint64_t>(info_.componentId) << ',';
    out << "\"uptime_s\":" << uptime;
    out << "},";
    out << "\"runtime\":{";
    out << "\"entity_count\":" << runtime.entityCount << ',';
    out << "\"session_count\":" << runtime.sessionCount << ',';
    out << "\"entity_types\":[";
    for (std::size_t i = 0; i < runtime.entityTypes.size(); ++i) {
        if (i > 0) out << ',';
        out << '"' << escapeJson(runtime.entityTypes[i]) << '"';
    }
    out << "],";
    out << "\"transport\":{";
    out << "\"messages_sent\":" << runtime.transportStats.messagesSent << ',';
    out << "\"messages_received\":" << runtime.transportStats.messagesReceived << ',';
    out << "\"bytes_sent\":" << runtime.transportStats.bytesSent << ',';
    out << "\"bytes_received\":" << runtime.transportStats.bytesReceived << ',';
    out << "\"outbound_queue_depth\":" << runtime.transportStats.outboundQueueDepth << ',';
    out << "\"inbound_queue_depth\":" << runtime.transportStats.inboundQueueDepth << ',';
    out << "\"backpressure_events\":" << runtime.transportStats.backPressureEvents;
    out << '}';
    out << '}';
    out << '}';
    return out.str();
}

std::string OpsInspector::renderEntitiesJson() const {
    const auto runtime = snapshot();
    std::ostringstream out;
    out << '{';
    out << "\"entity_count\":" << runtime.entityCount << ',';
    out << "\"entity_types\":[";
    for (std::size_t i = 0; i < runtime.entityTypes.size(); ++i) {
        if (i > 0) out << ',';
        out << '"' << escapeJson(runtime.entityTypes[i]) << '"';
    }
    out << "]}";
    return out.str();
}

}  // namespace theseed::ops
