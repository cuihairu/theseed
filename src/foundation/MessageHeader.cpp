#include "theseed/foundation/MessageHeader.h"
#include "theseed/foundation/MemoryStream.h"

namespace theseed::foundation {

void encodeHeader(const MessageHeader& header, MemoryStream& stream) {
    stream.writeUint16(header.messageId);
    stream.writeUint32(header.payloadLength);
    stream.writeUint32(header.sequence);
    stream.writeUint8(static_cast<std::uint8_t>(header.delivery));
}

bool decodeHeader(MessageHeader& header, MemoryStream& stream) {
    if (stream.readRemaining() < MessageHeader::kEncodedSize) {
        return false;
    }
    header.messageId = stream.readUint16();
    header.payloadLength = stream.readUint32();
    header.sequence = stream.readUint32();
    header.delivery = static_cast<DeliveryFlag>(stream.readUint8());
    return true;
}

}  // namespace theseed::foundation
