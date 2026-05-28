#pragma once

#include "theseed/runtime/IBytePipe.h"
#include "theseed/runtime/InvocationCodec.h"
#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/foundation/ChannelRouter.h"
#include "theseed/foundation/MemoryStream.h"
#include "theseed/foundation/MessageHeader.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

namespace theseed::runtime {

class NetworkTransport final : public IRuntimeTransport {
public:
    static constexpr std::uint16_t kInvocationMessageId = 1;
    static constexpr std::uint16_t kHeartbeatMessageId = 2;

    struct Config {
        ComponentId localComponent{0};
        std::size_t maxMessageSize{64 * 1024};
    };

    explicit NetworkTransport(std::shared_ptr<IBytePipe> pipe)
        : NetworkTransport(pipe, Config{}) {}

    explicit NetworkTransport(std::shared_ptr<IBytePipe> pipe, Config config);
    ~NetworkTransport();

    NetworkTransport(const NetworkTransport&) = delete;
    NetworkTransport& operator=(const NetworkTransport&) = delete;

    // IRuntimeTransport
    SendResult send(RuntimeInvocation invocation) override;
    std::size_t receive(ComponentId targetComponent,
                        RuntimeInvocation* out,
                        std::size_t capacity) override;
    std::size_t pendingCount() const override;
    void flush() override;
    TransportStats stats() const override;

    bool isConnected() const;
    void close();
    void tick();

private:
    void flushOutbound();
    void onRawReceived(std::span<const std::byte> data);
    void parseInbound();
    bool parseOneMessage();
    void onPipeClosed();

    std::shared_ptr<IBytePipe> pipe_;
    Config config_;

    foundation::ChannelRouter router_;
    foundation::MemoryStream recvBuffer_;

    mutable std::mutex inboxMutex_;
    std::deque<RuntimeInvocation> inbox_;

    TransportStats stats_;
    TimePoint lastSendTime_{};
};

}  // namespace theseed::runtime
