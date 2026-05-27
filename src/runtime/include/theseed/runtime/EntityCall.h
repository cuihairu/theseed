#pragma once

#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/RuntimeTypes.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace theseed::runtime {

class IRuntimeTransport;

class EntityCall final {
public:
    EntityCall() = default;
    EntityCall(EntityId entityId,
               ComponentId targetComponent,
               std::string entityType,
               DeliveryClass deliveryClass = DeliveryClass::ORDERED_RELIABLE);

    EntityId entityId() const;
    bool isValid() const;

    ComponentId targetComponent() const;
    const std::string& entityType() const;
    DeliveryClass deliveryClass() const;

    RuntimeInvocation buildInvocation(std::string method,
                                      std::span<const std::byte> payload = {}) const;
    SendResult call(IRuntimeTransport& transport,
                    std::string method,
                    std::span<const std::byte> payload = {}) const;

    void updateTarget(ComponentId targetComponent);
    void invalidate();
    void setDeliveryClass(DeliveryClass deliveryClass);

private:
    EntityId entityId_ = 0;
    std::optional<ComponentId> targetComponent_{};
    std::string entityType_;
    DeliveryClass deliveryClass_ = DeliveryClass::ORDERED_RELIABLE;
};

}  // namespace theseed::runtime
