#include "theseed/runtime/NetworkTransport.h"

#include <cstring>
#include <utility>

namespace theseed::runtime {

NetworkTransport::NetworkTransport(std::shared_ptr<IBytePipe> pipe, Config config)
    : pipe_(std::move(pipe)),
      config_(config),
      recvBuffer_(4096) {
    if (pipe_) {
        pipe_->setOnReceived([this](std::span<const std::byte> data) {
            onRawReceived(data);
        });
    }
}

NetworkTransport::~NetworkTransport() {
    close();
}

SendResult NetworkTransport::send(RuntimeInvocation invocation) {
    if (!pipe_ || !pipe_->isConnected()) {
        return SendResult::NotConnected;
    }

    auto encoded = InvocationCodec::encode(invocation);
    if (encoded.size() > config_.maxMessageSize) {
        return SendResult::Oversized;
    }

    auto channelClass = invocation.deliveryClass == DeliveryClass::UNORDERED_LOSSY
                            ? ChannelClass::Unreliable
                            : ChannelClass::Reliable;

    foundation::ChannelRouter::RoutingKey routingKey{
        invocation.targetComponent,
        static_cast<std::uint8_t>(channelClass),
    };

    auto& channel = router_.getOrCreate(routingKey);
    if (channelClass == ChannelClass::Unreliable) {
        channel.setOverflowPolicy(foundation::Channel::OverflowPolicy::DiscardOldest);
    }

    if (channel.isBackPressured() &&
        channel.overflowPolicy() == foundation::Channel::OverflowPolicy::BackPressure) {
        ++stats_.backPressureEvents;
        return SendResult::BackPressure;
    }

    foundation::Bundle bundle;
    auto delivery = invocation.deliveryClass == DeliveryClass::UNORDERED_LOSSY
                        ? foundation::DeliveryFlag::UnorderedLossy
                        : foundation::DeliveryFlag::OrderedReliable;
    bundle.beginMessage(kInvocationMessageId, static_cast<std::uint8_t>(channelClass), delivery);
    bundle.stream().writeBytes(encoded.data(), encoded.size());
    bundle.endMessage();

    channel.send(std::move(bundle));

    ++stats_.messagesSent;
    stats_.bytesSent += encoded.size();

    flushOutbound();
    lastSendTime_ = Clock::now();
    return SendResult::Accepted;
}

std::size_t NetworkTransport::receive(ComponentId targetComponent,
                                       RuntimeInvocation* out,
                                       std::size_t capacity) {
    if (capacity == 0) return 0;

    std::lock_guard lock(inboxMutex_);
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
    stats_.messagesReceived += count;
    return count;
}

std::size_t NetworkTransport::pendingCount() const {
    std::lock_guard lock(inboxMutex_);
    return inbox_.size();
}

bool NetworkTransport::isConnected() const {
    return pipe_ && pipe_->isConnected();
}

void NetworkTransport::close() {
    if (pipe_) {
        pipe_->setOnReceived(nullptr);
        pipe_->close();
    }
}

void NetworkTransport::flush() {
    flushOutbound();
}

TransportStats NetworkTransport::stats() const {
    std::lock_guard lock(inboxMutex_);
    auto s = stats_;
    s.inboundQueueDepth = inbox_.size();
    s.outboundQueueDepth = router_.totalPendingCount();
    return s;
}

void NetworkTransport::tick() {
    // Read incoming data from the wire
    if (pipe_) {
        pipe_->pump();
    }

    flushOutbound();

    if (isConnected()) {
        auto now = Clock::now();
        auto elapsed = std::chrono::duration_cast<Duration>(now - lastSendTime_);
        if (elapsed >= std::chrono::seconds{30}) {
            foundation::ChannelRouter::RoutingKey ctrlKey{0, static_cast<std::uint8_t>(ChannelClass::Control)};
            auto& ctrlChannel = router_.getOrCreate(ctrlKey);
            foundation::Bundle hb;
            hb.beginMessage(kHeartbeatMessageId, static_cast<std::uint8_t>(ChannelClass::Control),
                            foundation::DeliveryFlag::OrderedReliable);
            hb.endMessage();
            ctrlChannel.send(std::move(hb));
            flushOutbound();
            lastSendTime_ = now;
        }
    }
}

void NetworkTransport::flushOutbound() {
    if (!pipe_ || !pipe_->isConnected()) return;

    foundation::MemoryStream framed;
    if (router_.drainAll(framed) > 0) {
        pipe_->write(std::span<const std::byte>(framed.data(), framed.size()));
    }
}

void NetworkTransport::onRawReceived(std::span<const std::byte> data) {
    if (data.empty()) return;
    stats_.bytesReceived += data.size();
    recvBuffer_.writeBytes(data.data(), data.size());
    parseInbound();
}

void NetworkTransport::parseInbound() {
    while (parseOneMessage()) {
        // continue
    }
}

bool NetworkTransport::parseOneMessage() {
    recvBuffer_.resetRead();
    if (recvBuffer_.readRemaining() < foundation::MessageHeader::kEncodedSize) {
        return false;
    }

    auto savedReadPos = recvBuffer_.readPos();

    foundation::MessageHeader header;
    if (!foundation::decodeHeader(header, recvBuffer_)) {
        recvBuffer_.resetRead();
        recvBuffer_.readSkip(savedReadPos);
        return false;
    }

    if (recvBuffer_.readRemaining() < header.payloadLength) {
        // Not enough data for the full payload yet.
        // Restore read position so we can try again later.
        // Since decodeHeader already advanced readPos, we need to compact.
        recvBuffer_.resetRead();
        return false;
    }

    // We have a complete message. Extract payload.
    std::vector<std::byte> payload(header.payloadLength);
    if (header.payloadLength > 0) {
        recvBuffer_.readBytes(payload.data(), header.payloadLength);
    }

    // Calculate how much we consumed total
    const auto consumed = foundation::MessageHeader::kEncodedSize + header.payloadLength;
    auto remaining = recvBuffer_.size() - consumed;

    // Compact recvBuffer: move unconsumed data to front
    if (remaining > 0) {
        const auto* src = recvBuffer_.data() + consumed;
        std::vector<std::byte> leftover(src, src + remaining);
        recvBuffer_.clear();
        recvBuffer_.writeBytes(leftover.data(), leftover.size());
    } else {
        recvBuffer_.clear();
    }

    // Dispatch by message ID
    if (header.messageId == kInvocationMessageId && !payload.empty()) {
        auto invocation = InvocationCodec::decode(payload);
        std::lock_guard lock(inboxMutex_);
        inbox_.push_back(std::move(invocation));
    }
    // kHeartbeatMessageId: no-op, already consumed

    return true;
}

void NetworkTransport::onPipeClosed() {
    // Future: notify upper layers
}

}  // namespace theseed::runtime
