#include "theseed/foundation/Bundle.h"

#include <cstring>

namespace theseed::foundation {

void Bundle::beginMessage(std::uint16_t messageId, DeliveryFlag delivery) {
    MessageHeader header;
    header.messageId = messageId;
    header.payloadLength = 0;
    header.sequence = delivery == DeliveryFlag::OrderedReliable ? nextSequence_++ : 0;
    header.delivery = delivery;

    headerPos_ = stream_.writePos();
    encodeHeader(header, stream_);
    inMessage_ = true;
}

void Bundle::endMessage() {
    if (!inMessage_) return;

    const auto currentPos = stream_.writePos();
    const auto payloadStart = headerPos_ + MessageHeader::kEncodedSize;
    const auto payloadLength = static_cast<std::uint32_t>(currentPos - payloadStart);

    const auto lengthOffset = headerPos_ + sizeof(std::uint16_t);
    std::memcpy(stream_.data() + lengthOffset, &payloadLength, sizeof(payloadLength));

    ++messageCount_;
    inMessage_ = false;
}

MemoryStream& Bundle::stream() { return stream_; }
const MemoryStream& Bundle::stream() const { return stream_; }
std::size_t Bundle::messageCount() const { return messageCount_; }

void Bundle::clear() {
    stream_.clear();
    messageCount_ = 0;
    headerPos_ = 0;
    inMessage_ = false;
}

}  // namespace theseed::foundation
