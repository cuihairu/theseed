#pragma once

#include "theseed/core/IEntityStore.h"
#include "theseed/foundation/TimerWheel.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/GroupManager.h"
#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/TickScheduler.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace theseed::core {

using foundation::TimerHandle;
using foundation::TimerWheel;

class BaseRuntime final : public runtime::ITickable {
public:
    using EntityFactory = std::function<std::unique_ptr<runtime::Entity>(
        runtime::EntityId, runtime::EntitySide)>;

    BaseRuntime(std::shared_ptr<runtime::IRuntimeTransport> transport,
                std::shared_ptr<IEntityStore> store,
                runtime::ComponentId localComponentId);

    void attach(runtime::TickScheduler& scheduler);
    void detach(runtime::TickScheduler& scheduler);

    bool registerEntityFactory(const std::string& entityType, EntityFactory factory);

    runtime::Entity* createEntity(const std::string& entityType);
    runtime::Entity* loadEntity(runtime::EntityId id, const std::string& entityType);
    std::size_t restoreEntities(const std::string& entityType);
    bool destroyEntity(runtime::EntityId id);
    runtime::Entity* findEntity(runtime::EntityId id) const;
    std::vector<runtime::Entity*> findEntitiesByType(const std::string& entityType) const;
    std::vector<runtime::Entity*> findEntitiesByTag(const std::string& tag) const;
    std::vector<runtime::Entity*> queryEntities(std::function<bool(const runtime::Entity&)> predicate) const;
    void forEachEntity(std::function<void(runtime::Entity&)> callback) const;
    std::size_t entityCount() const;

    bool setCellEntityCall(runtime::EntityId id, runtime::ComponentId cellComponent);
    bool clearCellEntityCall(runtime::EntityId id);

    bool saveEntity(runtime::EntityId id);
    void setAutoSaveInterval(runtime::Duration interval);

    TimerHandle addTimer(TimerWheel::Duration delay, TimerWheel::Callback callback);
    TimerHandle addEntityTimer(runtime::EntityId entityId, TimerWheel::Duration delay, TimerWheel::Callback callback);
    TimerHandle addEntityPeriodicTimer(runtime::EntityId entityId, TimerWheel::Duration interval, TimerWheel::Callback callback);
    bool cancelTimer(TimerHandle handle);
    void cancelEntityTimers(runtime::EntityId entityId);

    std::size_t pumpInbound();
    bool dispatchInvocation(const runtime::RuntimeInvocation& invocation);

    using AoIEnterCallback = std::function<void(runtime::EntityId observerId,
                                                  runtime::EntityId targetId,
                                                  const std::string& targetType,
                                                  bool hasPosition,
                                                  const runtime::Vector3& position)>;
    using AoILeaveCallback = std::function<void(runtime::EntityId observerId,
                                                  runtime::EntityId targetId)>;
    using ClientEventCallback = std::function<void(runtime::EntityId entityId,
                                                    const std::string& eventName,
                                                    std::span<const std::byte> data)>;
    using WitnessSyncCallback = std::function<void(runtime::EntityId observerId,
                                                    runtime::EntityId targetEntityId,
                                                    std::span<const std::byte> propertyData,
                                                    bool hasPosition,
                                                    const runtime::Vector3& position)>;
    using SpaceChangeCallback = std::function<void(runtime::EntityId entityId,
                                                    runtime::SpaceId spaceId,
                                                    const runtime::Vector3& position)>;
    using DestructionCallback = std::function<void(runtime::EntityId entityId,
                                                    const std::string& entityType)>;
    using EntityFactoryHook = std::function<void(runtime::Entity& entity)>;

    void setEntityFactoryHook(EntityFactoryHook hook);

    void setOnAoIEnter(AoIEnterCallback cb);
    void setOnAoILeave(AoILeaveCallback cb);
    void setOnClientEvent(ClientEventCallback cb);
    void setOnWitnessSync(WitnessSyncCallback cb);
    void setOnSpaceChange(SpaceChangeCallback cb);
    void setOnEntityDestroyed(DestructionCallback cb);

    runtime::GroupManager& groupManager();
    const runtime::GroupManager& groupManager() const;

    bool requestCreateCell(runtime::EntityId entityId,
                           const std::string& entityType,
                           const runtime::Vector3& position,
                           runtime::ComponentId targetCellApp,
                           runtime::SpaceId spaceId = 0);
    bool requestDestroyCell(runtime::EntityId entityId,
                            runtime::ComponentId targetCellApp);
    bool requestTeleport(runtime::EntityId entityId,
                         runtime::SpaceId targetSpaceId,
                         const runtime::Vector3& position);

    void tick(runtime::TickContext& context) override;

private:
    class IngressPump final : public runtime::ITickable {
    public:
        explicit IngressPump(BaseRuntime& owner);
        void tick(runtime::TickContext& context) override;

    private:
        BaseRuntime* owner_ = nullptr;
    };

    class TimerPump final : public runtime::ITickable {
    public:
        explicit TimerPump(BaseRuntime& owner);
        void tick(runtime::TickContext& context) override;

    private:
        BaseRuntime* owner_ = nullptr;
    };

    class SyncBuildPump final : public runtime::ITickable {
    public:
        explicit SyncBuildPump(BaseRuntime& owner);
        void tick(runtime::TickContext& context) override;

    private:
        BaseRuntime* owner_ = nullptr;
    };

    class FlushPump final : public runtime::ITickable {
    public:
        explicit FlushPump(BaseRuntime& owner);
        void tick(runtime::TickContext& context) override;

    private:
        BaseRuntime* owner_ = nullptr;
    };

    struct PendingRuntimeSync final {
        runtime::RuntimeInvocation invocation;
        runtime::EntityId clearDirtyEntityId = 0;
    };

    void advanceTimers(runtime::TickContext& context);
    void buildSync(runtime::TickContext& context);
    void flushRuntimeTransport();
    void autoSaveAll();

    bool handleCellReady(const runtime::RuntimeInvocation& invocation);
    bool handleCellDestroyed(const runtime::RuntimeInvocation& invocation);
    bool handlePropertySyncFromCell(const runtime::RuntimeInvocation& invocation);
    bool handleAoIEnter(const runtime::RuntimeInvocation& invocation);
    bool handleAoILeave(const runtime::RuntimeInvocation& invocation);
    bool handleSpawnRequest(const runtime::RuntimeInvocation& invocation);
    bool handleClientEvent(const runtime::RuntimeInvocation& invocation);
    bool handleWitnessSync(const runtime::RuntimeInvocation& invocation);
    bool handleSpaceChanged(const runtime::RuntimeInvocation& invocation);

    void syncToCells();
    void completeBaseDestruction(runtime::EntityId entityId);

    std::shared_ptr<runtime::IRuntimeTransport> transport_;
    std::shared_ptr<IEntityStore> store_;
    runtime::ComponentId localComponentId_ = 0;
    IngressPump ingressPump_;
    TimerPump timerPump_;
    SyncBuildPump syncBuildPump_;
    FlushPump flushPump_;

    std::unordered_map<std::string, EntityFactory> factories_;
    std::unordered_map<std::string, std::shared_ptr<runtime::EntityDef>> defs_;
    std::unordered_map<runtime::EntityId, std::unique_ptr<runtime::Entity>> entities_;
    std::unordered_map<runtime::EntityId, std::vector<TimerHandle>> entityTimers_;
    std::vector<PendingRuntimeSync> pendingRuntimeSync_;
    TimerWheel timerWheel_;
    runtime::Duration autoSaveInterval_{};
    runtime::Duration autoSaveAccumulator_{};
    AoIEnterCallback onAoIEnter_;
    AoILeaveCallback onAoILeave_;
    ClientEventCallback onClientEvent_;
    WitnessSyncCallback onWitnessSync_;
    SpaceChangeCallback onSpaceChange_;
    DestructionCallback onEntityDestroyed_;
    EntityFactoryHook factoryHook_;
    std::unordered_set<runtime::EntityId> pendingDestructions_;
    runtime::GroupManager groupManager_;
};

}  // namespace theseed::core
