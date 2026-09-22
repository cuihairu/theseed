#pragma once

#include "theseed/runtime/RuntimeTypes.h"

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace theseed::runtime {

struct RuntimeInvocation final {
    EntityId entityId = 0;
    ComponentId sourceComponent = 0;
    ComponentId targetComponent = 0;
    std::string entityType;
    std::string method;
    DeliveryClass deliveryClass = DeliveryClass::ORDERED_RELIABLE;
    std::vector<std::byte> payload;
};

class IRuntimeTransport {
public:
    virtual ~IRuntimeTransport() = default;

    virtual SendResult send(RuntimeInvocation invocation) = 0;
    virtual std::size_t receive(ComponentId targetComponent,
                                RuntimeInvocation* out,
                                std::size_t capacity) = 0;
    virtual std::size_t pendingCount() const = 0;
    virtual void flush() = 0;
    virtual TransportStats stats() const = 0;

    // 周期驱动：读管道入站、冲刷出站。TCP 子类在 tick 里 pump socket，
    // 内存实现为空操作。TransportHub::tick 借此驱动所有 peer 的入站——
    // 缺了这一环，服务端永远读不到 socket 上的请求。
    virtual void tick() {}
};

class InMemoryRuntimeTransport final : public IRuntimeTransport {
public:
    InMemoryRuntimeTransport() = default;

    InMemoryRuntimeTransport(const InMemoryRuntimeTransport&) = delete;
    InMemoryRuntimeTransport& operator=(const InMemoryRuntimeTransport&) = delete;

    SendResult send(RuntimeInvocation invocation) override;
    std::size_t receive(ComponentId targetComponent,
                        RuntimeInvocation* out,
                        std::size_t capacity) override;
    std::size_t pendingCount() const override;
    void flush() override;
    TransportStats stats() const override;
    void tick() override {}

    std::size_t drain(RuntimeInvocation* out, std::size_t capacity);

private:
    mutable std::mutex mutex_;
    std::deque<RuntimeInvocation> invocations_;
    TransportStats stats_;
};

}  // namespace theseed::runtime
