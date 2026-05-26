#include "theseed/runtime/PipedTransport.h"
#include "theseed/runtime/InvocationCodec.h"

namespace theseed::runtime {

PipedTransport::PipedTransport(ComponentId localComponent)
    : localComponent_(localComponent) {}

bool PipedTransport::send(RuntimeInvocation invocation) {
    if (peer_ == nullptr) {
        return false;
    }

    auto bytes = InvocationCodec::encode(invocation);
    auto decoded = InvocationCodec::decode(bytes);

    std::lock_guard lock(peer_->mutex_);
    peer_->inbox_.push_back(std::move(decoded));
    return true;
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

void PipedTransport::connect(PipedTransport& peer) {
    peer_ = &peer;
    peer.peer_ = this;
}

void PipedTransport::flush() {
    static_cast<void>(this);
}

}  // namespace theseed::runtime
