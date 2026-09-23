#include "theseed/runtime/TransportHub.h"

#include <cstddef>
#include <mutex>
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

void TransportHub::attachServerTransport(std::shared_ptr<IRuntimeTransport> transport) {
    if (!transport) return;
    std::lock_guard lock(mutex_);
    awaitingIdentity_.push_back(std::move(transport));
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

    // 服务端连接自学习：首条入站消息的 sourceComponent 即对端身份，
    // 以此注册进 peers_，后续回复才能按 target=sourceComponent 路由回去。
    for (auto it = awaitingIdentity_.begin(); it != awaitingIdentity_.end();) {
        if (total >= capacity) break;
        auto count = (*it)->receive(targetComponent, out + total, capacity - total);
        if (count > 0 && out[total].sourceComponent != 0) {
            peers_.try_emplace(out[total].sourceComponent, *it);
            it = awaitingIdentity_.erase(it);
        } else {
            ++it;
        }
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
    for (const auto& transport : awaitingIdentity_) {
        total += transport->pendingCount();
    }
    return total;
}

void TransportHub::flush() {
    std::lock_guard lock(mutex_);
    for (auto& [_, transport] : peers_) {
        transport->flush();
    }
    for (auto& transport : awaitingIdentity_) {
        transport->flush();
    }
}

TransportStats TransportHub::stats() const {
    std::lock_guard lock(mutex_);
    TransportStats aggregated;
    auto accumulate = [&aggregated](const std::shared_ptr<IRuntimeTransport>& transport) {
        auto s = transport->stats();
        aggregated.messagesSent += s.messagesSent;
        aggregated.messagesReceived += s.messagesReceived;
        aggregated.bytesSent += s.bytesSent;
        aggregated.bytesReceived += s.bytesReceived;
        aggregated.outboundQueueDepth += s.outboundQueueDepth;
        aggregated.inboundQueueDepth += s.inboundQueueDepth;
        aggregated.backPressureEvents += s.backPressureEvents;
    };
    for (const auto& [_, transport] : peers_) {
        accumulate(transport);
    }
    for (const auto& transport : awaitingIdentity_) {
        accumulate(transport);
    }
    return aggregated;
}

void TransportHub::tick() {
    std::lock_guard lock(mutex_);
    for (auto& [_, transport] : peers_) {
        // tick 负责 pump 入站字节并冲刷出站；补一次 flush 保证纯队列实现
        // （无 tick 逻辑）也能推出待发数据。
        transport->tick();
        transport->flush();
    }
    for (auto& transport : awaitingIdentity_) {
        transport->tick();
        transport->flush();
    }
}

}  // namespace theseed::runtime
