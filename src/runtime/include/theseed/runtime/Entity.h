#pragma once

#include "theseed/runtime/EntityCall.h"
#include "theseed/runtime/PropertyBlock.h"
#include "theseed/runtime/RuntimeTransport.h"

#include <atomic>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace theseed::runtime {

enum class EntitySide : std::uint8_t {
    Base = 0,
    Cell,
};

enum class EntityState : std::uint8_t {
    Creating = 0,
    Active,
    Migrating,
    Destroying,
    Destroyed,
};

class Entity final {
public:
    Entity(EntityId id, EntitySide side, const EntityDef& def);

    EntityId id() const;
    EntitySide side() const;
    EntityState state() const;
    const std::string& entityType() const;

    const EntityCall* baseEntityCall() const;
    EntityCall* baseEntityCall();
    const EntityCall* cellEntityCall() const;
    EntityCall* cellEntityCall();

    void bindBaseEntityCall(ComponentId targetComponent,
                            DeliveryClass deliveryClass = DeliveryClass::ORDERED_RELIABLE);
    void bindCellEntityCall(ComponentId targetComponent,
                            DeliveryClass deliveryClass = DeliveryClass::ORDERED_RELIABLE);
    void clearBaseEntityCall();
    void clearCellEntityCall();

    using MethodHandler = std::function<void(Entity&, std::span<const std::byte>)>;
    using LifecycleCallback = std::function<void(Entity&)>;
    using SpaceCallback = std::function<void(Entity&, SpaceId)>;
    using AoICallback = std::function<void(Entity&, EntityId)>;
    using EventCallback = std::function<void(Entity&, std::string_view event, std::span<const std::byte> data)>;

    bool bindMethodHandler(std::string method, MethodHandler handler);
    bool hasMethodHandler(std::string_view method) const;
    bool dispatchMethod(std::string_view method, std::span<const std::byte> payload);
    bool dispatchInvocation(const RuntimeInvocation& invocation);
    void clearMethodHandlers();

    void subscribe(std::string event, EventCallback callback);
    void unsubscribe(const std::string& event);
    void emit(std::string_view event, std::span<const std::byte> data = {});

    void setOnCreate(LifecycleCallback cb);
    void setOnDestroy(LifecycleCallback cb);
    void setOnEnterSpace(SpaceCallback cb);
    void setOnLeaveSpace(SpaceCallback cb);
    void setOnEnterAoI(AoICallback cb);
    void setOnLeaveAoI(AoICallback cb);

    void notifyCreate();
    void notifyDestroy();
    void notifyEnterSpace(SpaceId spaceId);
    void notifyLeaveSpace(SpaceId spaceId);
    void notifyEnterAoI(EntityId other);
    void notifyLeaveAoI(EntityId other);

    void activate();
    void beginMigration();
    void beginDestroy();
    void destroy();

    template <typename T>
    const T& getProperty(PropertyId id) const {
        return properties_.get<T>(id);
    }

    template <typename T>
    void setProperty(PropertyId id, const T& value) {
        properties_.set<T>(id, value);
    }

    bool isPropertyDirty(PropertyId id) const;
    void clearDirtyFlags();
    std::vector<PropertyDelta> buildDirtyPropertyDelta() const;
    void applyPropertyDelta(std::span<const PropertyDelta> deltas, bool markDirty = false);

    const PropertyBlock& propertyBlock() const;
    PropertyBlock& propertyBlock();

    void addTag(std::string tag);
    void removeTag(const std::string& tag);
    bool hasTag(const std::string& tag) const;
    const std::unordered_set<std::string>& tags() const;

private:
    EntityId id_ = 0;
    EntitySide side_ = EntitySide::Base;
    std::atomic<EntityState> state_{EntityState::Creating};
    const EntityDef* def_ = nullptr;
    std::optional<EntityCall> baseCall_{};
    std::optional<EntityCall> cellCall_{};
    std::unordered_map<std::string, MethodHandler> methodHandlers_{};
    std::unordered_multimap<std::string, EventCallback> eventSubscriptions_;
    LifecycleCallback onCreate_;
    LifecycleCallback onDestroy_;
    SpaceCallback onEnterSpace_;
    SpaceCallback onLeaveSpace_;
    AoICallback onEnterAoI_;
    AoICallback onLeaveAoI_;
    std::unordered_set<std::string> tags_;
    PropertyBlock properties_;
};

}  // namespace theseed::runtime
