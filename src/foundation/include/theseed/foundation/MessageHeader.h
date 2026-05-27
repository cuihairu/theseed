#pragma once

#include <cstddef>
#include <cstdint>

namespace theseed::foundation {

enum class DeliveryFlag : std::uint8_t {
    OrderedReliable = 0,
    UnorderedLossy = 1,
};

struct MessageHeader final {
    std::uint16_t messageId = 0;
    std::uint32_t payloadLength = 0;
    std::uint32_t sequence = 0;
    std::uint8_t channelClass = 0;
    DeliveryFlag delivery = DeliveryFlag::OrderedReliable;

    static constexpr std::size_t kEncodedSize =
        sizeof(std::uint16_t) + sizeof(std::uint32_t) + sizeof(std::uint32_t) +
        sizeof(std::uint8_t) + sizeof(std::uint8_t);

    friend bool operator==(const MessageHeader&, const MessageHeader&) = default;
};

class MemoryStream;

void encodeHeader(const MessageHeader& header, MemoryStream& stream);
bool decodeHeader(MessageHeader& header, MemoryStream& stream);

}  // namespace theseed::foundation
