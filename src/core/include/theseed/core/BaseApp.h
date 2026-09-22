#pragma once

#include "theseed/core/BaseRuntime.h"
#include "theseed/core/EntityDefRegistry.h"
#include "theseed/login/ClientProtocol.h"
#include "theseed/login/ClientSession.h"
#include "theseed/login/LoginProtocol.h"
#include "theseed/ops/OpsInspector.h"
#include "theseed/ops/OpsServer.h"
#include "theseed/runtime/PropertyReplication.h"
#include "theseed/runtime/RuntimeLoop.h"
#include "theseed/runtime/TcpListener.h"
#include "theseed/runtime/TransportStatsCollector.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace theseed::core {

class BaseApp final {
public:
    struct OpsConfig final {
        bool enabled = false;
        std::string host = "127.0.0.1";
        std::uint16_t port = 20010;
        std::size_t maxConnections = 8;
    };

    struct Config {
        std::string entityDefPath;
        runtime::ComponentId componentId = 1;
        runtime::Duration autoSaveInterval = std::chrono::seconds{30};
        std::string clientListenHost = "0.0.0.0";
        std::uint16_t clientListenPort = 20000;
        OpsConfig ops;
    };

    BaseApp(Config config,
            std::shared_ptr<runtime::IRuntimeTransport> transport,
            std::shared_ptr<IEntityStore> store);

    bool init();
    void attach(runtime::TickScheduler& scheduler);
    void detach(runtime::TickScheduler& scheduler);
    void tick();

    BaseRuntime& runtime();
    const BaseRuntime& runtime() const;
    EntityDefRegistry& registry();
    const EntityDefRegistry& registry() const;

    // init() 后可用：实际绑定的客户端监听端口（Config.clientListenPort 为 0 时由系统分配）
    std::uint16_t clientListenPort() const;

    // init() 后可用：实际绑定的 ops 监听端口（仅 ops.enabled 时有意义）
    std::uint16_t opsListenPort() const;

    login::ClientSession* findSessionByEntity(runtime::EntityId entityId) const;
    void bindSessionToEntity(login::ClientSession* session, runtime::EntityId entityId);
    void takeClientSession(std::unique_ptr<login::ClientSession> session);

    runtime::Entity* createEntity(const std::string& entityType);
    runtime::Entity* findEntity(runtime::EntityId id) const;
    bool destroyEntity(runtime::EntityId id);
    std::size_t restoreEntities(const std::string& entityType);

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

    void handleClientAction(const login::ActionMsg& msg);

private:
    class ClientPump final : public runtime::ITickable {
    public:
        explicit ClientPump(BaseApp& owner);
        void tick(runtime::TickContext& context) override;

    private:
        BaseApp* owner_ = nullptr;
    };

    void acceptClientConnections();
    void onClientMessage(login::ClientSession* session,
                         login::ClientMessageType type,
                         std::span<const std::byte> payload);
    void handleEnterGame(login::ClientSession* session, const std::string& token);
    void flushClientPropertyUpdates();
    void cleanupClients();

    void onAoIEnter(runtime::EntityId observerId, runtime::EntityId targetId,
                     const std::string& targetType, bool hasPosition,
                     const runtime::Vector3& position);
    void onAoILeave(runtime::EntityId observerId, runtime::EntityId targetId);
    void onClientEvent(runtime::EntityId entityId, const std::string& eventName,
                       std::span<const std::byte> data);
    void onWitnessPropertySync(runtime::EntityId observerId, runtime::EntityId targetEntityId,
                               std::span<const std::byte> propertyData,
                               bool hasPosition, const runtime::Vector3& position);
    void onSpaceChanged(runtime::EntityId entityId, runtime::SpaceId spaceId,
                        const runtime::Vector3& position);
    void onEntityDestroyed(runtime::EntityId entityId, const std::string& entityType);

    Config config_;
    EntityDefRegistry registry_;
    std::unique_ptr<BaseRuntime> runtime_;
    std::shared_ptr<runtime::IRuntimeTransport> transport_;
    std::shared_ptr<IEntityStore> store_;
    ClientPump clientPump_;

    runtime::TcpListener clientListener_;
    std::vector<std::unique_ptr<login::ClientSession>> clientSessions_;
    std::unordered_map<login::ClientSession*, runtime::EntityId> sessionEntityMap_;
    std::unordered_map<runtime::EntityId, login::ClientSession*> entitySessionMap_;
    runtime::TransportStatsCollector transportStatsCollector_;

    std::unique_ptr<ops::OpsInspector> opsInspector_;
    std::unique_ptr<ops::OpsServer> opsServer_;
};

}  // namespace theseed::core
