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
    ComponentId targetComponent = 0;
    std::string entityType;
    std::string method;
    DeliveryClass deliveryClass = DeliveryClass::ORDERED_RELIABLE;
    std::vector<std::byte> payload;
};

class IRuntimeTransport {
public:
    virtual ~IRuntimeTransport() = default;

    virtual bool send(RuntimeInvocation invocation) = 0;
    virtual std::size_t receive(ComponentId targetComponent,
                                RuntimeInvocation* out,
                                std::size_t capacity) = 0;
    virtual std::size_t pendingCount() const = 0;
};

class InMemoryRuntimeTransport final : public IRuntimeTransport {
public:
    InMemoryRuntimeTransport() = default;

    InMemoryRuntimeTransport(const InMemoryRuntimeTransport&) = delete;
    InMemoryRuntimeTransport& operator=(const InMemoryRuntimeTransport&) = delete;

    bool send(RuntimeInvocation invocation) override;
    std::size_t receive(ComponentId targetComponent,
                        RuntimeInvocation* out,
                        std::size_t capacity) override;
    std::size_t pendingCount() const override;

    std::size_t drain(RuntimeInvocation* out, std::size_t capacity);

private:
    mutable std::mutex mutex_;
    std::deque<RuntimeInvocation> invocations_;
};

}  // namespace theseed::runtime
