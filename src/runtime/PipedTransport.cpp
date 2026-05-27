#include "theseed/runtime/PipedTransport.h"
#include "theseed/runtime/InvocationCodec.h"

namespace theseed::runtime {

PipedTransport::PipedTransport(ComponentId localComponent)
    : localComponent_(localComponent) {}

SendResult PipedTransport::send(RuntimeInvocation invocation) {
    if (peer_ == nullptr) {
        return SendResult::NotConnected;
    }

    auto bytes = InvocationCodec::encode(invocation);
    auto decoded = InvocationCodec::decode(bytes);

    std::lock_guard lock(peer_->mutex_);
    ++peer_->stats_.messagesSent;
    peer_->inbox_.push_back(std::move(decoded));
    return SendResult::Accepted;
}

std::size_t PipedTransport::receive(ComponentId targetComponent,
                                     RuntimeInvocation* out,
                                     std::size_t capacity) {
    std::lock_guard lock(mutex_);
    std::size_t count = 0;

    auto it = inbox_.begin();
    while (it != inbox_.end() && count < capacity) {
        if (it->targetComponent == targetComponent) {
            out[count] = std::move(*it);
            it = inbox_.erase(it);
            ++count;
        } else {
            ++it;
        }
    }

    return count;
}

std::size_t PipedTransport::pendingCount() const {
    std::lock_guard lock(mutex_);
    return inbox_.size();
}

void PipedTransport::flush() {
    // PipedTransport has no outbound buffering.
}

TransportStats PipedTransport::stats() const {
    std::lock_guard lock(mutex_);
    auto s = stats_;
    s.inboundQueueDepth = inbox_.size();
    return s;
}

void PipedTransport::connect(PipedTransport& peer) {
    peer_ = &peer;
    peer.peer_ = this;
}

}  // namespace theseed::runtime
