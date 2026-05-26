#pragma once

#include "theseed/runtime/GhostManager.h"
#include "theseed/runtime/EntityMigration.h"
#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/SpaceRuntime.h"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>

namespace theseed::foundation {
struct TimerHandle;
class TimerWheel;
}  // namespace theseed::foundation

namespace theseed::runtime {

class CellRuntime final : public ITickable {
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
    void removeEntity(EntityId entityId);
    Entity* findEntity(EntityId entityId) const;

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

    using TimerCallback = std::function<void()>;

    foundation::TimerHandle addTimer(Duration delay, TimerCallback callback);
    bool cancelTimer(foundation::TimerHandle handle);

    void broadcastEvent(std::string_view event, std::span<const std::byte> data = {});
    void broadcastEventInRange(std::string_view event, const Vector3& center, float range,
                               std::span<const std::byte> data = {});

    void tick(TickContext& context) override;

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

    class FlushPump final : public ITickable {
    public:
        explicit FlushPump(CellRuntime& owner);
        void tick(TickContext& context) override;

    private:
        CellRuntime* owner_ = nullptr;
    };

    void syncRealGhosts();
    bool applyMigrationTransfer(const RuntimeInvocation& invocation);
    bool applyMigrationCommit(const RuntimeInvocation& invocation);
    bool routeMigratingInvocation(const RuntimeInvocation& invocation);

    struct GhostBinding final {
        std::unique_ptr<GhostManager> manager;
    };

    std::unique_ptr<SpaceRuntime> spaceRuntime_;
    std::shared_ptr<IRuntimeTransport> transport_;
    ComponentId localComponentId_ = 0;
    IngressPump ingressPump_;
    FlushPump flushPump_;
    std::unordered_map<std::string, EntityFactory> entityFactories_;
    std::unordered_map<EntityId, std::unique_ptr<Entity>> ownedEntities_;
    std::unordered_map<EntityId, MigrationRoute> migrationRoutes_;
    std::unordered_map<EntityId, GhostBinding> ghostBindings_;
    std::unique_ptr<foundation::TimerWheel> timerWheel_;
};

}  // namespace theseed::runtime
