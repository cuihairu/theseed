#pragma once

#include "theseed/foundation/Bundle.h"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>

namespace theseed::foundation {

class Channel final {
public:
    enum class OverflowPolicy : std::uint8_t {
        BackPressure,
        DiscardOldest,
    };

    struct Watermark {
        std::size_t low = 64;
        std::size_t high = 256;
    };

    explicit Channel(std::uint32_t targetComponent);

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    void send(Bundle bundle);
    bool drain(MemoryStream& out);
    bool hasPending() const;
    std::size_t pendingBundleCount() const;

    std::uint32_t targetComponent() const;
    std::uint32_t nextSequence() const;

    void setWatermark(Watermark wm);
    void setOverflowPolicy(OverflowPolicy policy);
    bool isBackPressured() const;
    const Watermark& watermark() const;
    OverflowPolicy overflowPolicy() const;

private:
    std::uint32_t targetComponent_;
    std::uint32_t sequence_ = 0;
    std::deque<Bundle> outbound_;
    Watermark watermark_;
    OverflowPolicy overflowPolicy_ = OverflowPolicy::BackPressure;
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

}  // namespace theseed::foundation
