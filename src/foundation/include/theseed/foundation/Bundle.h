#pragma once

#include "theseed/foundation/MessageHeader.h"
#include "theseed/foundation/MemoryStream.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace theseed::foundation {

class Bundle final {
public:
    Bundle() = default;

    Bundle(const Bundle&) = delete;
    Bundle& operator=(const Bundle&) = delete;
    Bundle(Bundle&&) = default;
    Bundle& operator=(Bundle&&) = default;

    void beginMessage(std::uint16_t messageId, std::uint8_t channelClass = 0,
                      DeliveryFlag delivery = DeliveryFlag::OrderedReliable);
    void endMessage();

    MemoryStream& stream();
    const MemoryStream& stream() const;

    std::size_t messageCount() const;

    void clear();

private:
    MemoryStream stream_;
    std::size_t messageCount_ = 0;

    // Tracks where the current message header was written for backfill
    std::size_t headerPos_ = 0;
    std::uint32_t nextSequence_ = 0;
    bool inMessage_ = false;
};

}  // namespace theseed::foundation
