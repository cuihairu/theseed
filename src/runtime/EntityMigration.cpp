#include "theseed/runtime/EntityMigration.h"

#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace theseed::runtime {

namespace {

template <typename T>
void appendValue(std::vector<std::byte>& buffer, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto offset = buffer.size();
    buffer.resize(offset + sizeof(T));
    std::memcpy(buffer.data() + offset, &value, sizeof(T));
}

template <typename T>
T readValue(std::span<const std::byte> payload, std::size_t& cursor) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (cursor + sizeof(T) > payload.size()) {
        throw std::invalid_argument("migration payload is truncated");
    }

    T value{};
    std::memcpy(&value, payload.data() + cursor, sizeof(T));
    cursor += sizeof(T);
    return value;
}

void appendBytes(std::vector<std::byte>& buffer, std::span<const std::byte> bytes) {
    const auto size = static_cast<std::uint32_t>(bytes.size());
    appendValue(buffer, size);
    if (bytes.empty()) {
        return;
    }

    const auto offset = buffer.size();
    buffer.resize(offset + bytes.size());
    std::memcpy(buffer.data() + offset, bytes.data(), bytes.size());
}

std::vector<std::byte> readBytes(std::span<const std::byte> payload, std::size_t& cursor) {
    const auto size = readValue<std::uint32_t>(payload, cursor);
    if (cursor + size > payload.size()) {
        throw std::invalid_argument("migration payload bytes are truncated");
    }

    std::vector<std::byte> bytes(size);
    if (size > 0) {
        std::memcpy(bytes.data(), payload.data() + cursor, size);
    }
    cursor += size;
    return bytes;
}

void appendString(std::vector<std::byte>& buffer, const std::string& value) {
    const auto size = static_cast<std::uint32_t>(value.size());
    appendValue(buffer, size);
    if (value.empty()) {
        return;
    }

    const auto offset = buffer.size();
    buffer.resize(offset + value.size());
    std::memcpy(buffer.data() + offset, value.data(), value.size());
}

std::string readString(std::span<const std::byte> payload, std::size_t& cursor) {
    const auto size = readValue<std::uint32_t>(payload, cursor);
    if (cursor + size > payload.size()) {
        throw std::invalid_argument("migration payload string is truncated");
    }

    std::string value(size, '\0');
    if (size > 0) {
        std::memcpy(value.data(), payload.data() + cursor, size);
    }
    cursor += size;
    return value;
}

void appendRouteBinding(std::vector<std::byte>& buffer, const std::optional<RouteBinding>& route) {
    appendValue<std::uint8_t>(buffer, route.has_value() ? 1U : 0U);
    if (!route.has_value()) {
        return;
    }

    appendValue(buffer, route->targetComponent);
    appendValue(buffer, static_cast<std::uint8_t>(route->deliveryClass));
}

std::optional<RouteBinding> readRouteBinding(std::span<const std::byte> payload,
                                             std::size_t& cursor) {
    const auto present = readValue<std::uint8_t>(payload, cursor);
    if (present == 0) {
        return std::nullopt;
    }

    RouteBinding route;
    route.targetComponent = readValue<ComponentId>(payload, cursor);
    route.deliveryClass =
        static_cast<DeliveryClass>(readValue<std::uint8_t>(payload, cursor));
    return route;
}

}  // namespace

EntityMigrationSnapshot EntityMigration::capture(const Entity& entity,
                                                 MigrationEpoch epoch,
                                                 ComponentId sourceComponent,
                                                 ComponentId targetComponent,
                                                 std::optional<Vector3> position,
                                                 SpaceId spaceId) {
    EntityMigrationSnapshot snapshot;
    snapshot.entityId = entity.id();
    snapshot.side = entity.side();
    snapshot.entityType = entity.entityType();
    snapshot.epoch = epoch;
    snapshot.sourceComponent = sourceComponent;
    snapshot.targetComponent = targetComponent;
    snapshot.spaceId = spaceId;
    snapshot.position = position;

    const auto& block = entity.propertyBlock();
    snapshot.propertyStorage.resize(block.size());
    if (!snapshot.propertyStorage.empty()) {
        std::memcpy(snapshot.propertyStorage.data(), block.data(), block.size());
    }

    if (const auto* baseCall = entity.baseEntityCall(); baseCall != nullptr) {
        snapshot.baseCall = RouteBinding{
            .targetComponent = baseCall->targetComponent(),
            .deliveryClass = baseCall->deliveryClass(),
        };
    }

    if (const auto* cellCall = entity.cellEntityCall(); cellCall != nullptr) {
        snapshot.cellCall = RouteBinding{
            .targetComponent = cellCall->targetComponent(),
            .deliveryClass = cellCall->deliveryClass(),
        };
    }

    return snapshot;
}

void EntityMigration::restore(Entity& entity, const EntityMigrationSnapshot& snapshot) {
    if (entity.id() != snapshot.entityId) {
        throw std::invalid_argument("migration snapshot entity id mismatch");
    }

    if (entity.entityType() != snapshot.entityType) {
        throw std::invalid_argument("migration snapshot entity type mismatch");
    }

    auto& block = entity.propertyBlock();
    if (block.size() != snapshot.propertyStorage.size()) {
        throw std::invalid_argument("migration snapshot property size mismatch");
    }

    if (!snapshot.propertyStorage.empty()) {
        std::memcpy(block.data(), snapshot.propertyStorage.data(), snapshot.propertyStorage.size());
    }

    if (snapshot.baseCall.has_value()) {
        entity.bindBaseEntityCall(snapshot.baseCall->targetComponent,
                                  snapshot.baseCall->deliveryClass);
    } else {
        entity.clearBaseEntityCall();
    }

    if (snapshot.cellCall.has_value()) {
        entity.bindCellEntityCall(snapshot.cellCall->targetComponent,
                                  snapshot.cellCall->deliveryClass);
    } else {
        entity.clearCellEntityCall();
    }

    entity.clearDirtyFlags();
    entity.activate();
}

std::vector<std::byte> EntityMigration::encode(const EntityMigrationSnapshot& snapshot) {
    std::vector<std::byte> payload;
    payload.reserve(sizeof(snapshot.entityId) + sizeof(snapshot.epoch) + snapshot.entityType.size() +
                    snapshot.propertyStorage.size() + 64);

    appendValue(payload, snapshot.entityId);
    appendValue(payload, static_cast<std::uint8_t>(snapshot.side));
    appendString(payload, snapshot.entityType);
    appendValue(payload, snapshot.epoch);
    appendValue(payload, snapshot.sourceComponent);
    appendValue(payload, snapshot.targetComponent);
    appendValue(payload, snapshot.spaceId);
    appendValue<std::uint8_t>(payload, snapshot.position.has_value() ? 1U : 0U);
    if (snapshot.position.has_value()) {
        appendValue(payload, snapshot.position->x);
        appendValue(payload, snapshot.position->y);
        appendValue(payload, snapshot.position->z);
    }
    appendBytes(payload, snapshot.propertyStorage);
    appendRouteBinding(payload, snapshot.baseCall);
    appendRouteBinding(payload, snapshot.cellCall);
    return payload;
}

EntityMigrationSnapshot EntityMigration::decode(std::span<const std::byte> payload) {
    EntityMigrationSnapshot snapshot;
    std::size_t cursor = 0;

    snapshot.entityId = readValue<EntityId>(payload, cursor);
    snapshot.side = static_cast<EntitySide>(readValue<std::uint8_t>(payload, cursor));
    snapshot.entityType = readString(payload, cursor);
    snapshot.epoch = readValue<MigrationEpoch>(payload, cursor);
    snapshot.sourceComponent = readValue<ComponentId>(payload, cursor);
    snapshot.targetComponent = readValue<ComponentId>(payload, cursor);
    snapshot.spaceId = readValue<SpaceId>(payload, cursor);

    const auto hasPosition = readValue<std::uint8_t>(payload, cursor);
    if (hasPosition != 0) {
        snapshot.position = Vector3{
            .x = readValue<float>(payload, cursor),
            .y = readValue<float>(payload, cursor),
            .z = readValue<float>(payload, cursor),
        };
    }

    snapshot.propertyStorage = readBytes(payload, cursor);
    snapshot.baseCall = readRouteBinding(payload, cursor);
    snapshot.cellCall = readRouteBinding(payload, cursor);

    if (cursor != payload.size()) {
        throw std::invalid_argument("migration payload contains unexpected trailing bytes");
    }

    return snapshot;
}

}  // namespace theseed::runtime
