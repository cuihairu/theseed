#include "theseed/runtime/NetworkNode.h"
#include "theseed/runtime/TcpConnection.h"

#include <utility>

namespace theseed::runtime {

NetworkNode::NetworkNode(Config config)
    : config_(std::move(config)),
      hub_(std::make_shared<TransportHub>(config_.localComponent)) {
    if (config_.listenPort > 0 || config_.listenHost == "127.0.0.1" || config_.listenHost == "0.0.0.0") {
        listener_.listen(config_.listenHost, config_.listenPort);
    }
}

NetworkNode::~NetworkNode() {
    if (scheduler_) {
        detach(*scheduler_);
    }
    transports_.clear();
    listener_.close();
}

void NetworkNode::attach(TickScheduler& scheduler) {
    scheduler_ = &scheduler;
    scheduler.registerTickable(TickPhase::Network, *this);
}

void NetworkNode::detach(TickScheduler& scheduler) {
    scheduler.unregisterTickable(TickPhase::Network, *this);
    scheduler_ = nullptr;
}

bool NetworkNode::connectToPeer(ComponentId peerId,
                                 const std::string& host,
                                 std::uint16_t port) {
    auto conn = TcpConnection::create();
    if (!conn->connect(host, port)) {
        return false;
    }

    auto transport = std::make_shared<NetworkTransport>(conn);
    transports_[peerId] = transport;
    hub_->connectPeer(peerId, transport);
    return true;
}

void NetworkNode::disconnectPeer(ComponentId peerId) {
    hub_->disconnectPeer(peerId);
    transports_.erase(peerId);
}

void NetworkNode::acceptPeer(ComponentId peerId,
                              std::shared_ptr<IRuntimeTransport> transport) {
    auto nt = std::dynamic_pointer_cast<NetworkTransport>(transport);
    if (nt) {
        transports_[peerId] = nt;
    }
    hub_->connectPeer(peerId, std::move(transport));
}

std::shared_ptr<TransportHub> NetworkNode::hub() {
    return hub_;
}

void NetworkNode::setOnPeerConnected(PeerCallback callback) {
    onPeerConnected_ = std::move(callback);
}

std::uint16_t NetworkNode::listenPort() const {
    return listener_.localPort();
}

bool NetworkNode::hasPeer(ComponentId peerId) const {
    return hub_->hasPeer(peerId);
}

std::size_t NetworkNode::peerCount() const {
    return hub_->peerCount();
}

void NetworkNode::tick(TickContext& context) {
    static_cast<void>(context);

    acceptIncoming();

    for (auto& [_, transport] : transports_) {
        transport->tick();
    }
}

void NetworkNode::acceptIncoming() {
    if (!listener_.isListening()) return;

    while (true) {
        auto conn = listener_.accept();
        if (!conn) break;

        auto transport = std::make_shared<NetworkTransport>(conn);
        auto rawPtr = conn.get();

        // Notify callback; caller assigns peerId via acceptPeer()
        if (onPeerConnected_) {
            onPeerConnected_(transport);
        }
        // Without callback, the connection is accepted but not routed.
        // Caller should use connectToPeer for bidirectional setup.
    }
}

}  // namespace theseed::runtime
