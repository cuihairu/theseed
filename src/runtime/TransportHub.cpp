#include "theseed/runtime/TransportHub.h"

#include <utility>

namespace theseed::runtime {

TransportHub::TransportHub(ComponentId localComponent)
    : localComponent_(localComponent) {}

void TransportHub::connectPeer(ComponentId peerId,
                                std::shared_ptr<IRuntimeTransport> transport) {
    if (!transport || peerId == 0) return;
    std::lock_guard lock(mutex_);
    peers_[peerId] = std::move(transport);
}

void TransportHub::disconnectPeer(ComponentId peerId) {
    std::lock_guard lock(mutex_);
    peers_.erase(peerId);
}

bool TransportHub::hasPeer(ComponentId peerId) const {
    std::lock_guard lock(mutex_);
    return peers_.contains(peerId);
}

std::size_t TransportHub::peerCount() const {
    std::lock_guard lock(mutex_);
    return peers_.size();
}

SendResult TransportHub::send(RuntimeInvocation invocation) {
    std::lock_guard lock(mutex_);
    auto it = peers_.find(invocation.targetComponent);
    if (it == peers_.end()) {
        return SendResult::NotConnected;
    }
    return it->second->send(std::move(invocation));
}

std::size_t TransportHub::receive(ComponentId targetComponent,
                                   RuntimeInvocation* out,
                                   std::size_t capacity) {
    if (capacity == 0 || out == nullptr) return 0;

    std::lock_guard lock(mutex_);
    std::size_t total = 0;
    for (auto& [_, transport] : peers_) {
        if (total >= capacity) break;
        auto count = transport->receive(targetComponent, out + total, capacity - total);
        total += count;
    }
    return total;
}

std::size_t TransportHub::pendingCount() const {
    std::lock_guard lock(mutex_);
    std::size_t total = 0;
    for (const auto& [_, transport] : peers_) {
        total += transport->pendingCount();
    }
    return total;
}

void TransportHub::flush() {
    std::lock_guard lock(mutex_);
    for (auto& [_, transport] : peers_) {
        transport->flush();
    }
}

TransportStats TransportHub::stats() const {
    std::lock_guard lock(mutex_);
    TransportStats aggregated;
    for (const auto& [_, transport] : peers_) {
        auto s = transport->stats();
        aggregated.messagesSent += s.messagesSent;
        aggregated.messagesReceived += s.messagesReceived;
        aggregated.bytesSent += s.bytesSent;
        aggregated.bytesReceived += s.bytesReceived;
        aggregated.outboundQueueDepth += s.outboundQueueDepth;
        aggregated.inboundQueueDepth += s.inboundQueueDepth;
        aggregated.backPressureEvents += s.backPressureEvents;
    }
    return aggregated;
}

void TransportHub::tick() {
    std::lock_guard lock(mutex_);
    for (auto& [_, transport] : peers_) {
        transport->flush();
    }
}

}  // namespace theseed::runtime
