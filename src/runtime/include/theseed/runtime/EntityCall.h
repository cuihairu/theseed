#pragma once

#include "theseed/foundation/MemoryStream.h"
#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/RuntimeTypes.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <type_traits>

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

    template <typename... Args>
    SendResult callWith(IRuntimeTransport& transport, std::string method, const Args&... args) const {
        foundation::MemoryStream ms;
        (writeArg(ms, args), ...);
        return call(transport, std::move(method),
                    std::span<const std::byte>(ms.data(), ms.size()));
    }

    void updateTarget(ComponentId targetComponent);
    void invalidate();
    void setDeliveryClass(DeliveryClass deliveryClass);

private:
    static void writeArg(foundation::MemoryStream& ms, std::int8_t v) { ms.writeInt8(v); }
    static void writeArg(foundation::MemoryStream& ms, std::int16_t v) { ms.writeInt16(v); }
    static void writeArg(foundation::MemoryStream& ms, std::int32_t v) { ms.writeInt32(v); }
    static void writeArg(foundation::MemoryStream& ms, std::int64_t v) { ms.writeInt64(v); }
    static void writeArg(foundation::MemoryStream& ms, std::uint8_t v) { ms.writeUint8(v); }
    static void writeArg(foundation::MemoryStream& ms, std::uint16_t v) { ms.writeUint16(v); }
    static void writeArg(foundation::MemoryStream& ms, std::uint32_t v) { ms.writeUint32(v); }
    static void writeArg(foundation::MemoryStream& ms, std::uint64_t v) { ms.writeUint64(v); }
    static void writeArg(foundation::MemoryStream& ms, float v) { ms.writeFloat(v); }
    static void writeArg(foundation::MemoryStream& ms, double v) { ms.writeDouble(v); }
    static void writeArg(foundation::MemoryStream& ms, bool v) { ms.writeBool(v); }
    static void writeArg(foundation::MemoryStream& ms, const std::string& v) { ms.writeString(v); }
    static void writeArg(foundation::MemoryStream& ms, std::string_view v) { ms.writeString(v); }

    EntityId entityId_ = 0;
    std::optional<ComponentId> targetComponent_{};
    std::string entityType_;
    DeliveryClass deliveryClass_ = DeliveryClass::ORDERED_RELIABLE;
};

}  // namespace theseed::runtime
