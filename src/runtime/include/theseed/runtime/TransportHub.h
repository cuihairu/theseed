#pragma once

#include "theseed/runtime/RuntimeTransport.h"

#include <cstddef>
#include <mutex>
#include <unordered_map>

namespace theseed::runtime {

class TransportHub final : public IRuntimeTransport {
public:
    explicit TransportHub(ComponentId localComponent);

    TransportHub(const TransportHub&) = delete;
    TransportHub& operator=(const TransportHub&) = delete;

    void connectPeer(ComponentId peerId, std::shared_ptr<IRuntimeTransport> transport);
    void disconnectPeer(ComponentId peerId);
    bool hasPeer(ComponentId peerId) const;
    std::size_t peerCount() const;

    // IRuntimeTransport
    SendResult send(RuntimeInvocation invocation) override;
    std::size_t receive(ComponentId targetComponent,
                        RuntimeInvocation* out,
                        std::size_t capacity) override;
    std::size_t pendingCount() const override;
    void flush() override;
    TransportStats stats() const override;

    void tick();

private:
    ComponentId localComponent_;
    mutable std::mutex mutex_;
    std::unordered_map<ComponentId, std::shared_ptr<IRuntimeTransport>> peers_;
};

}  // namespace theseed::runtime
