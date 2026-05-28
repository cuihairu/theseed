#pragma once

#include "theseed/core/BaseRuntime.h"
#include "theseed/core/EntityDefRegistry.h"
#include "theseed/login/ClientProtocol.h"
#include "theseed/login/ClientSession.h"
#include "theseed/login/LoginProtocol.h"
#include "theseed/runtime/RuntimeLoop.h"
#include "theseed/runtime/TcpListener.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace theseed::core {

class BaseApp final {
public:
    struct Config {
        std::string entityDefPath;
        runtime::ComponentId componentId = 1;
        runtime::Duration autoSaveInterval = std::chrono::seconds{30};
        std::string clientListenHost = "0.0.0.0";
        std::uint16_t clientListenPort = 20000;
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

    runtime::Entity* createEntity(const std::string& entityType);
    runtime::Entity* findEntity(runtime::EntityId id) const;
    bool destroyEntity(runtime::EntityId id);
    std::size_t restoreEntities(const std::string& entityType);

    bool requestCreateCell(runtime::EntityId entityId,
                           const std::string& entityType,
                           const runtime::Vector3& position,
                           runtime::ComponentId targetCellApp);
    bool requestDestroyCell(runtime::EntityId entityId,
                            runtime::ComponentId targetCellApp);

private:
    void acceptClientConnections();
    void onClientMessage(login::ClientSession* session,
                         login::ClientMessageType type,
                         std::span<const std::byte> payload);
    void handleEnterGame(login::ClientSession* session, const std::string& token);
    void cleanupClients();

    Config config_;
    EntityDefRegistry registry_;
    std::unique_ptr<BaseRuntime> runtime_;
    std::shared_ptr<runtime::IRuntimeTransport> transport_;
    std::shared_ptr<IEntityStore> store_;

    runtime::TcpListener clientListener_;
    std::vector<std::unique_ptr<login::ClientSession>> clientSessions_;
    std::unordered_map<login::ClientSession*, runtime::EntityId> sessionEntityMap_;
};

}  // namespace theseed::core
