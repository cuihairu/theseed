#pragma once

#include "theseed/runtime/NetworkTransport.h"
#include "theseed/runtime/TcpListener.h"
#include "theseed/runtime/TickScheduler.h"
#include "theseed/runtime/TransportHub.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace theseed::runtime {

class NetworkNode final : public ITickable {
public:
    using PeerCallback = std::function<void(std::shared_ptr<IRuntimeTransport>)>;

    struct Config {
        ComponentId localComponent = 0;
        std::string listenHost = "127.0.0.1";
        std::uint16_t listenPort = 0;
    };

    explicit NetworkNode(Config config);
    ~NetworkNode();

    NetworkNode(const NetworkNode&) = delete;
    NetworkNode& operator=(const NetworkNode&) = delete;

    void attach(TickScheduler& scheduler);
    void detach(TickScheduler& scheduler);

    bool connectToPeer(ComponentId peerId, const std::string& host, std::uint16_t port);
    void acceptPeer(ComponentId peerId, std::shared_ptr<IRuntimeTransport> transport);
    void disconnectPeer(ComponentId peerId);

    std::shared_ptr<TransportHub> hub();
    void setOnPeerConnected(PeerCallback callback);

    std::uint16_t listenPort() const;
    bool hasPeer(ComponentId peerId) const;
    std::size_t peerCount() const;

    void tick(TickContext& context) override;

private:
    void acceptIncoming();

    Config config_;
    TcpListener listener_;
    std::shared_ptr<TransportHub> hub_;
    std::unordered_map<ComponentId, std::shared_ptr<NetworkTransport>> transports_;
    PeerCallback onPeerConnected_;
    TickScheduler* scheduler_ = nullptr;
};

}  // namespace theseed::runtime
