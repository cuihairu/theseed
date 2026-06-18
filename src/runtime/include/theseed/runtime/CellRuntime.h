#pragma once

#include "theseed/runtime/GhostManager.h"
#include "theseed/runtime/GroupManager.h"
#include "theseed/runtime/EntityMigration.h"
#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/SpaceRuntime.h"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace theseed::foundation {
struct TimerHandle;
class TimerWheel;
}  // namespace theseed::foundation

namespace theseed::runtime {

class CellRuntime final {
public:
    using EntityFactory = std::function<std::unique_ptr<Entity>(EntityId, EntitySide)>;

    CellRuntime(std::unique_ptr<SpaceRuntime> spaceRuntime,
                std::shared_ptr<IRuntimeTransport> transport,
                ComponentId localComponentId);
    ~CellRuntime();

    SpaceRuntime& spaceRuntime();
    const SpaceRuntime& spaceRuntime() const;
    IRuntimeTransport& transport();
    ComponentId localComponentId() const;

    void attach(TickScheduler& scheduler);
    void detach(TickScheduler& scheduler);

    void addEntity(Entity& entity, const Vector3& position);
    void addEntity(Entity& entity, const Vector3& position, SpaceId spaceId);
    void removeEntity(EntityId entityId);
    Entity* findEntity(EntityId entityId) const;
    std::vector<Entity*> findEntitiesByTag(const std::string& tag) const;
    std::vector<Entity*> queryEntities(std::function<bool(const Entity&)> predicate) const;
    void forEachEntity(std::function<void(Entity&)> callback) const;
    bool teleportEntity(EntityId entityId, SpaceId targetSpaceId, const Vector3& position);

    bool registerEntityFactory(std::string entityType, EntityFactory factory);
    bool beginMigration(EntityId entityId,
                        ComponentId targetComponent,
                        MigrationEpoch epoch);
    bool clearMigrationRoute(EntityId entityId);

    GhostManager& ensureRealGhost(Entity& entity, ComponentId ghostTarget);
    GhostManager& ensureGhostProxy(Entity& entity, ComponentId realTarget);
    GhostManager* findGhostManager(EntityId entityId) const;

    std::size_t pumpInbound();
    bool forwardGhostMethod(EntityId entityId,
                            std::string method,
                            std::span<const std::byte> payload = {});
    bool dispatchInvocation(const RuntimeInvocation& invocation);
    bool applyGhostSync(const RuntimeInvocation& invocation);
    bool handleCreateCell(const RuntimeInvocation& invocation);
    bool handleDestroyCell(const RuntimeInvocation& invocation);
    bool handlePropertySyncFromBase(const RuntimeInvocation& invocation);
    bool handleEntityAction(const RuntimeInvocation& invocation);
    bool handleTeleport(const RuntimeInvocation& invocation);

    bool requestSpawnEntity(const std::string& entityType,
                            const Vector3& position,
                            EntityId creatorEntityId);

    SpaceRuntime* findSpaceRuntime(SpaceId id) const;
    SpaceId findEntitySpace(EntityId id) const;
    bool createSpace(SpaceId id, std::string name);
    bool destroySpace(SpaceId id);

    using TimerCallback = std::function<void()>;

    foundation::TimerHandle addTimer(Duration delay, TimerCallback callback);
    foundation::TimerHandle addEntityTimer(EntityId entityId, Duration delay, TimerCallback callback);
    foundation::TimerHandle addEntityPeriodicTimer(EntityId entityId, Duration interval, TimerCallback callback);
    bool cancelTimer(foundation::TimerHandle handle);
    void cancelEntityTimers(EntityId entityId);

    void broadcastEvent(std::string_view event, std::span<const std::byte> data = {});
    void broadcastEventInRange(std::string_view event, const Vector3& center, float range,
                               std::span<const std::byte> data = {});

    using EntityFactoryHook = std::function<void(Entity& entity)>;
    void setEntityFactoryHook(EntityFactoryHook hook);

    GroupManager& groupManager();
    const GroupManager& groupManager() const;

    void tick(TickContext& context);

private:
    struct MigrationRoute final {
        ComponentId targetComponent = 0;
        MigrationEpoch epoch = 0;
        TimePoint expiry{};
    };

    class IngressPump final : public ITickable {
    public:
        explicit IngressPump(CellRuntime& owner);
        void tick(TickContext& context) override;

    private:
        CellRuntime* owner_ = nullptr;
    };

    class TimerPump final : public ITickable {
    public:
        explicit TimerPump(CellRuntime& owner);
        void tick(TickContext& context) override;

    private:
        CellRuntime* owner_ = nullptr;
    };

    class SyncBuildPump final : public ITickable {
    public:
        explicit SyncBuildPump(CellRuntime& owner);
        void tick(TickContext& context) override;

    private:
        CellRuntime* owner_ = nullptr;
    };

    class FlushPump final : public ITickable {
    public:
        explicit FlushPump(CellRuntime& owner);
        void tick(TickContext& context) override;

    private:
        CellRuntime* owner_ = nullptr;
    };

    void syncRealGhosts();
    void syncToBases();
    void flushClientEvents();
    void flushWitnessSync();
    void flushAoIEvents();
    void advanceTimers(TickContext& context);
    void buildSync(TickContext& context);
    void flushRuntimeTransport();
    bool applyMigrationTransfer(const RuntimeInvocation& invocation);
    bool applyMigrationCommit(const RuntimeInvocation& invocation);
    bool routeMigratingInvocation(const RuntimeInvocation& invocation);

    struct GhostBinding final {
        std::unique_ptr<GhostManager> manager;
    };

    SpaceId defaultSpaceId_ = 0;
    std::unordered_map<SpaceId, std::unique_ptr<SpaceRuntime>> spaceRuntimes_;
    std::unordered_map<EntityId, SpaceId> entitySpaceMap_;
    std::shared_ptr<IRuntimeTransport> transport_;
    ComponentId localComponentId_ = 0;
    TickScheduler* scheduler_ = nullptr;
    IngressPump ingressPump_;
    TimerPump timerPump_;
    SyncBuildPump syncBuildPump_;
    FlushPump flushPump_;
    std::vector<RuntimeInvocation> pendingRuntimeSync_;
    std::unordered_map<std::string, EntityFactory> entityFactories_;
    std::unordered_map<EntityId, std::unique_ptr<Entity>> ownedEntities_;
    std::unordered_map<EntityId, MigrationRoute> migrationRoutes_;
    std::unordered_map<EntityId, GhostBinding> ghostBindings_;
    std::unique_ptr<foundation::TimerWheel> timerWheel_;
    std::unordered_map<EntityId, std::vector<foundation::TimerHandle>> entityTimers_;
    EntityFactoryHook factoryHook_;
    GroupManager groupManager_;
};

}  // namespace theseed::runtime
