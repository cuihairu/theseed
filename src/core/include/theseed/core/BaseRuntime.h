#pragma once

#include "theseed/core/IEntityStore.h"
#include "theseed/foundation/TimerWheel.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/TickScheduler.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

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
    bool destroyEntity(runtime::EntityId id);
    runtime::Entity* findEntity(runtime::EntityId id) const;
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

    bool requestCreateCell(runtime::EntityId entityId,
                           const std::string& entityType,
                           const runtime::Vector3& position,
                           runtime::ComponentId targetCellApp);
    bool requestDestroyCell(runtime::EntityId entityId,
                            runtime::ComponentId targetCellApp);

    void tick(runtime::TickContext& context) override;

private:
    class IngressPump final : public runtime::ITickable {
    public:
        explicit IngressPump(BaseRuntime& owner);
        void tick(runtime::TickContext& context) override;

    private:
        BaseRuntime* owner_ = nullptr;
    };

    void autoSaveAll();

    bool handleCellReady(const runtime::RuntimeInvocation& invocation);
    bool handleCellDestroyed(const runtime::RuntimeInvocation& invocation);
    bool handlePropertySyncFromCell(const runtime::RuntimeInvocation& invocation);

    void syncToCells();

    std::shared_ptr<runtime::IRuntimeTransport> transport_;
    std::shared_ptr<IEntityStore> store_;
    runtime::ComponentId localComponentId_ = 0;
    IngressPump ingressPump_;

    std::unordered_map<std::string, EntityFactory> factories_;
    std::unordered_map<std::string, std::shared_ptr<runtime::EntityDef>> defs_;
    std::unordered_map<runtime::EntityId, std::unique_ptr<runtime::Entity>> entities_;
    std::unordered_map<runtime::EntityId, std::vector<TimerHandle>> entityTimers_;
    TimerWheel timerWheel_;
    runtime::Duration autoSaveInterval_{};
    runtime::Duration autoSaveAccumulator_{};
};

}  // namespace theseed::core
