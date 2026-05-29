#pragma once

#include "theseed/foundation/MemoryStream.h"
#include "theseed/foundation/TimerWheel.h"
#include "theseed/runtime/EntityCall.h"
#include "theseed/runtime/PropertyBlock.h"
#include "theseed/runtime/RuntimeTransport.h"

#include <atomic>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace theseed::runtime {

class ControllerManager;
struct Vector3;

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
    ~Entity();

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
    using StreamMethodHandler = std::function<void(Entity&, foundation::MemoryStream&)>;
    using LifecycleCallback = std::function<void(Entity&)>;
    using SpaceCallback = std::function<void(Entity&, SpaceId)>;
    using AoICallback = std::function<void(Entity&, EntityId)>;
    using EventCallback = std::function<void(Entity&, std::string_view event, std::span<const std::byte> data)>;
    using PositionCallback = std::function<void(Entity&, Vector3 oldPos, Vector3 newPos)>;

    struct InputAction {
        std::string name;
        std::vector<std::byte> payload;
    };

    using ActionHandler = std::function<void(Entity&, std::string_view action, std::span<const std::byte> payload)>;

    bool bindMethodHandler(std::string method, MethodHandler handler);
    bool bindStreamMethodHandler(std::string method, StreamMethodHandler handler);
    bool hasMethodHandler(std::string_view method) const;
    bool dispatchMethod(std::string_view method, std::span<const std::byte> payload);
    bool dispatchInvocation(const RuntimeInvocation& invocation);
    void clearMethodHandlers();

    void subscribe(std::string event, EventCallback callback);
    void unsubscribe(const std::string& event);
    void emit(std::string_view event, std::span<const std::byte> data = {});

    void pushInput(InputAction action);
    void processInput();
    void clearInput();
    std::size_t pendingInputCount() const;
    void setActionHandler(ActionHandler handler);

    void setOnCreate(LifecycleCallback cb);
    void setOnRestore(LifecycleCallback cb);
    void setOnDestroy(LifecycleCallback cb);
    void setOnCellReady(LifecycleCallback cb);
    void setOnEnterSpace(SpaceCallback cb);
    void setOnLeaveSpace(SpaceCallback cb);
    void setOnEnterAoI(AoICallback cb);
    void setOnLeaveAoI(AoICallback cb);
    void setOnPositionChanged(PositionCallback cb);

    void notifyCreate();
    void notifyRestore();
    void notifyDestroy();
    void notifyCellReady();
    void notifyEnterSpace(SpaceId spaceId);
    void notifyLeaveSpace(SpaceId spaceId);
    void notifyEnterAoI(EntityId other);
    void notifyLeaveAoI(EntityId other);
    void notifyPositionChanged(Vector3 oldPos, Vector3 newPos);

    void activate();
    void beginMigration();
    void beginDestroy();
    void destroy();

    bool isActive() const;

    template <typename T>
    const T& getProperty(PropertyId id) const {
        return properties_.get<T>(id);
    }

    template <typename T>
    void setProperty(PropertyId id, const T& value) {
        properties_.set<T>(id, value);
    }

    template <typename T>
    const T* findProperty(const std::string& name) const {
        auto* desc = def_->findProperty(name);
        if (!desc) return nullptr;
        return &properties_.get<T>(desc->id);
    }

    template <typename T>
    bool setProperty(const std::string& name, const T& value) {
        auto* desc = def_->findProperty(name);
        if (!desc) return false;
        properties_.set<T>(desc->id, value);
        return true;
    }

    std::string_view getString(PropertyId id) const;
    void setString(PropertyId id, std::string_view value);
    std::string_view findString(const std::string& name) const;
    bool setString(const std::string& name, std::string_view value);

    std::span<const std::byte> getBlob(PropertyId id) const;
    void setBlob(PropertyId id, std::span<const std::byte> value);

    bool isPropertyDirty(PropertyId id) const;
    void clearDirtyFlags();
    std::vector<PropertyDelta> buildDirtyPropertyDelta(PropertyFlag excludeFlags = PropertyFlag::None) const;
    std::vector<PropertyDelta> buildFullPropertySnapshot(PropertyFlag excludeFlags = PropertyFlag::None) const;
    void applyPropertyDelta(std::span<const PropertyDelta> deltas, bool markDirty = false);

    using PropertyChangeCallback = std::function<void(Entity&, PropertyId,
                                                      const std::byte*, const std::byte*, std::size_t)>;

    void setPropertyChangedCallback(PropertyId id, PropertyChangeCallback callback);
    void clearPropertyChangedCallbacks();

    template <typename T>
    void onPropertyChanged(PropertyId id, std::function<void(Entity&, T, T)> callback) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (!callback) {
            setPropertyChangedCallback(id, nullptr);
            return;
        }

        setPropertyChangedCallback(id,
            [cb = std::move(callback)](Entity& e, PropertyId,
                                       const std::byte* oldVal,
                                       const std::byte* newVal,
                                       std::size_t size) {
                if (size != sizeof(T)) return;
                T oldT{}, newT{};
                std::memcpy(&oldT, oldVal, sizeof(T));
                std::memcpy(&newT, newVal, sizeof(T));
                cb(e, oldT, newT);
            });
    }

    template <typename T>
    bool onPropertyChanged(const std::string& name, std::function<void(Entity&, T, T)> callback) {
        auto* desc = def_->findProperty(name);
        if (!desc) return false;
        onPropertyChanged<T>(desc->id, std::move(callback));
        return true;
    }

    const PropertyBlock& propertyBlock() const;
    PropertyBlock& propertyBlock();

    void addTag(std::string tag);
    void removeTag(const std::string& tag);
    bool hasTag(const std::string& tag) const;
    const std::unordered_set<std::string>& tags() const;

    void setVelocity(Vector3 velocity);
    Vector3 velocity() const;
    void clearVelocity();
    bool hasVelocity() const;

    void setParent(EntityId parentId);
    EntityId parent() const;
    bool hasParent() const;
    void addChild(EntityId childId);
    void removeChild(EntityId childId);
    const std::unordered_set<EntityId>& children() const;
    bool hasChildren() const;
    std::size_t childCount() const;

    void setTransport(IRuntimeTransport* transport);
    IRuntimeTransport* transport() const;

    SendResult callCell(std::string method, std::span<const std::byte> payload = {});
    SendResult callBase(std::string method, std::span<const std::byte> payload = {});

    template <typename... Args>
    SendResult callCellWith(std::string method, const Args&... args) {
        if (!transport_ || !cellCall_ || !cellCall_->isValid()) {
            return SendResult::NotConnected;
        }
        return cellCall_->callWith(*transport_, std::move(method), args...);
    }

    template <typename... Args>
    SendResult callBaseWith(std::string method, const Args&... args) {
        if (!transport_ || !baseCall_ || !baseCall_->isValid()) {
            return SendResult::NotConnected;
        }
        return baseCall_->callWith(*transport_, std::move(method), args...);
    }

    using EntityTimerCallback = std::function<void(Entity&)>;
    using TimerScheduleFn = std::function<foundation::TimerHandle(
        Duration delay, EntityTimerCallback callback)>;

    void setTimerScheduleFns(TimerScheduleFn oneShot, TimerScheduleFn periodic);

    foundation::TimerHandle addTimer(Duration delay, EntityTimerCallback callback);
    foundation::TimerHandle addPeriodicTimer(Duration interval, EntityTimerCallback callback);

    // Position (set by Space/SpaceRuntime, read by controllers)
    Vector3 position() const;
    void setPosition(Vector3 pos);
    bool hasPosition() const;

    // Position query for other entities (set by SpaceRuntime)
    using PositionProvider = std::function<std::optional<Vector3>(EntityId)>;
    void setPositionProvider(PositionProvider provider);
    std::optional<Vector3> queryEntityPosition(EntityId entityId) const;

    // Controller system
    using ControllerId = std::uint32_t;
    using ControllerCallback = std::function<void(Entity&, ControllerId, std::int32_t, bool)>;

    ControllerManager& controllers();
    const ControllerManager& controllers() const;

    ControllerId moveTo(const Vector3& target, float speed,
                        float arrivalThreshold = 0.5F, std::int32_t userArg = 0);
    ControllerId moveToEntity(EntityId targetId, float speed,
                              float range = 1.0F, std::int32_t userArg = 0);
    void cancelController(ControllerId id);

    void setOnControllerComplete(ControllerCallback cb);
    void notifyControllerComplete(ControllerId id, std::int32_t userArg, bool success);

private:
    EntityId id_ = 0;
    EntitySide side_ = EntitySide::Base;
    std::atomic<EntityState> state_{EntityState::Creating};
    const EntityDef* def_ = nullptr;
    std::optional<EntityCall> baseCall_{};
    std::optional<EntityCall> cellCall_{};
    std::unordered_map<std::string, MethodHandler> methodHandlers_{};
    std::unordered_map<std::string, StreamMethodHandler> streamHandlers_{};
    std::unordered_multimap<std::string, EventCallback> eventSubscriptions_;
    std::vector<InputAction> inputQueue_;
    ActionHandler actionHandler_;
    LifecycleCallback onCreate_;
    LifecycleCallback onRestore_;
    LifecycleCallback onDestroy_;
    LifecycleCallback onCellReady_;
    SpaceCallback onEnterSpace_;
    SpaceCallback onLeaveSpace_;
    AoICallback onEnterAoI_;
    AoICallback onLeaveAoI_;
    PositionCallback onPositionChanged_;
    std::unordered_set<std::string> tags_;
    PropertyBlock properties_;
    IRuntimeTransport* transport_ = nullptr;
    TimerScheduleFn addOneShotTimer_;
    TimerScheduleFn addPeriodicTimerFn_;
    EntityId parentId_ = 0;
    std::unordered_set<EntityId> children_;
    Vector3 velocity_{};
    bool hasVelocity_ = false;
    Vector3 position_{};
    bool hasPosition_ = false;
    PositionProvider positionProvider_;
    mutable std::unique_ptr<ControllerManager> controllers_;
    ControllerCallback onControllerComplete_;
};

}  // namespace theseed::runtime
