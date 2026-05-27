#include "theseed/runtime/EntityCall.h"
#include "theseed/runtime/RuntimeTransport.h"

#include <cstring>
#include <span>
#include <stdexcept>
#include <utility>

namespace theseed::runtime {

EntityCall::EntityCall(EntityId entityId,
                       ComponentId targetComponent,
                       std::string entityType,
                       DeliveryClass deliveryClass)
    : entityId_(entityId),
      targetComponent_(targetComponent),
      entityType_(std::move(entityType)),
      deliveryClass_(deliveryClass) {}

EntityId EntityCall::entityId() const {
    return entityId_;
}

bool EntityCall::isValid() const {
    return targetComponent_.has_value();
}

ComponentId EntityCall::targetComponent() const {
    if (!targetComponent_) {
        throw std::logic_error("entity call target is invalid");
    }

    return *targetComponent_;
}

const std::string& EntityCall::entityType() const {
    return entityType_;
}

DeliveryClass EntityCall::deliveryClass() const {
    return deliveryClass_;
}

RuntimeInvocation EntityCall::buildInvocation(std::string method,
                                              std::span<const std::byte> payload) const {
    if (!isValid()) {
        throw std::logic_error("entity call target is invalid");
    }

    if (method.empty()) {
        throw std::invalid_argument("entity call method is empty");
    }

    RuntimeInvocation invocation;
    invocation.entityId = entityId_;
    invocation.targetComponent = targetComponent();
    invocation.entityType = entityType_;
    invocation.method = std::move(method);
    invocation.deliveryClass = deliveryClass_;
    invocation.payload.assign(payload.begin(), payload.end());
    return invocation;
}

SendResult EntityCall::call(IRuntimeTransport& transport,
                            std::string method,
                            std::span<const std::byte> payload) const {
    if (!isValid() || method.empty()) {
        return SendResult::Closed;
    }

    return transport.send(buildInvocation(std::move(method), payload));
}

void EntityCall::updateTarget(ComponentId targetComponent) {
    targetComponent_ = targetComponent;
}

void EntityCall::invalidate() {
    targetComponent_.reset();
}

void EntityCall::setDeliveryClass(DeliveryClass deliveryClass) {
    deliveryClass_ = deliveryClass;
}

}  // namespace theseed::runtime
