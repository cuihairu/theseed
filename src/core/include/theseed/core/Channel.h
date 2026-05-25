#pragma once

#include "theseed/core/Bundle.h"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>

namespace theseed::core {

class Channel final {
public:
    explicit Channel(std::uint32_t targetComponent);

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    void send(Bundle bundle);
    bool drain(MemoryStream& out);
    bool hasPending() const;
    std::size_t pendingBundleCount() const;

    std::uint32_t targetComponent() const;
    std::uint32_t nextSequence() const;

private:
    std::uint32_t targetComponent_;
    std::uint32_t sequence_ = 0;
    std::deque<Bundle> outbound_;
};

class IMessageHandler {
public:
    virtual ~IMessageHandler() = default;
    virtual bool handleMessage(std::uint16_t messageId, MemoryStream& payload) = 0;
};

class MessageDispatcher final {
public:
    using HandlerPtr = IMessageHandler*;

    void registerHandler(std::uint16_t messageId, HandlerPtr handler);
    void unregisterHandler(std::uint16_t messageId);
    bool dispatch(std::uint16_t messageId, MemoryStream& payload) const;

    std::size_t handlerCount() const;

private:
    static constexpr std::size_t kMaxMessageId = 1024;
    std::array<HandlerPtr, kMaxMessageId> handlers_{};
};

}  // namespace theseed::core
