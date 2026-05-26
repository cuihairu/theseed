#include "theseed/runtime/InvocationCodec.h"
#include "theseed/foundation/MemoryStream.h"

#include <cstring>
#include <stdexcept>

namespace theseed::runtime {

std::vector<std::byte> InvocationCodec::encode(const RuntimeInvocation& invocation) {
    foundation::MemoryStream stream;

    stream.writeUint64(invocation.entityId);
    stream.writeUint32(invocation.targetComponent);
    stream.writeString(invocation.entityType);
    stream.writeString(invocation.method);
    stream.writeUint8(static_cast<std::uint8_t>(invocation.deliveryClass));
    stream.writeUint32(static_cast<std::uint32_t>(invocation.payload.size()));
    if (!invocation.payload.empty()) {
        stream.writeBytes(invocation.payload.data(), invocation.payload.size());
    }

    const auto size = stream.writePos();
    std::vector<std::byte> result(size);
    std::memcpy(result.data(), stream.data(), size);
    return result;
}

RuntimeInvocation InvocationCodec::decode(std::span<const std::byte> data) {
    foundation::MemoryStream stream;
    stream.writeBytes(data.data(), data.size());
    stream.resetRead();

    RuntimeInvocation invocation;
    invocation.entityId = stream.readUint64();
    invocation.targetComponent = stream.readUint32();
    invocation.entityType = stream.readString();
    invocation.method = stream.readString();
    invocation.deliveryClass = static_cast<DeliveryClass>(stream.readUint8());

    const auto payloadLen = stream.readUint32();
    invocation.payload.resize(payloadLen);
    if (payloadLen > 0) {
        stream.readBytes(invocation.payload.data(), payloadLen);
    }

    return invocation;
}

}  // namespace theseed::runtime
