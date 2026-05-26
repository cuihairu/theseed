#include "theseed/foundation/Channel.h"

namespace theseed::foundation {

Channel::Channel(std::uint32_t targetComponent)
    : targetComponent_(targetComponent) {}

void Channel::send(Bundle bundle) {
    outbound_.push_back(std::move(bundle));
}

bool Channel::drain(MemoryStream& out) {
    if (outbound_.empty()) return false;

    for (auto& bundle : outbound_) {
        auto& bundleStream = bundle.stream();
        bundleStream.resetRead();

        const auto size = bundleStream.size();
        if (size > 0) {
            out.writeBytes(bundleStream.readPtr(), size);
        }
    }
    outbound_.clear();
    return true;
}

bool Channel::hasPending() const { return !outbound_.empty(); }
std::size_t Channel::pendingBundleCount() const { return outbound_.size(); }
std::uint32_t Channel::targetComponent() const { return targetComponent_; }
std::uint32_t Channel::nextSequence() const { return sequence_; }

void MessageDispatcher::registerHandler(std::uint16_t messageId, HandlerPtr handler) {
    if (messageId < kMaxMessageId) {
        handlers_[messageId] = handler;
    }
}

void MessageDispatcher::unregisterHandler(std::uint16_t messageId) {
    if (messageId < kMaxMessageId) {
        handlers_[messageId] = nullptr;
    }
}

bool MessageDispatcher::dispatch(std::uint16_t messageId, MemoryStream& payload) const {
    if (messageId >= kMaxMessageId) return false;
    auto* handler = handlers_[messageId];
    if (!handler) return false;
    return handler->handleMessage(messageId, payload);
}

std::size_t MessageDispatcher::handlerCount() const {
    std::size_t count = 0;
    for (const auto& h : handlers_) {
        if (h) ++count;
    }
    return count;
}

}  // namespace theseed::foundation
