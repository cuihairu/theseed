#include "theseed/core/BaseApp.h"
#include "theseed/login/SessionToken.h"
#include "theseed/runtime/TcpConnection.h"

#include <algorithm>
#include <stdexcept>

namespace theseed::core {

BaseApp::BaseApp(Config config,
                 std::shared_ptr<runtime::IRuntimeTransport> transport,
                 std::shared_ptr<IEntityStore> store)
    : config_(std::move(config)),
      transport_(std::move(transport)),
      store_(std::move(store)) {
    if (!transport_) {
        throw std::invalid_argument("base app requires transport");
    }
    if (!store_) {
        throw std::invalid_argument("base app requires entity store");
    }

    clientListener_.setConnectionFactory([]() {
        return runtime::TcpConnection::create();
    });
}

bool BaseApp::init() {
    if (!config_.entityDefPath.empty()) {
        registry_.loadDirectory(config_.entityDefPath);
    }

    runtime_ = std::make_unique<BaseRuntime>(transport_, store_, config_.componentId);

    for (const auto& entityType : registry_.entityTypes()) {
        auto factory = registry_.createFactory(entityType);
        if (factory) {
            runtime_->registerEntityFactory(entityType, std::move(factory));
        }
    }

    runtime_->setAutoSaveInterval(config_.autoSaveInterval);

    for (const auto& entityType : store_->listEntityTypes()) {
        if (runtime_->findEntitiesByType(entityType).empty()) {
            runtime_->restoreEntities(entityType);
        }
    }

    clientListener_.listen(config_.clientListenHost, config_.clientListenPort);

    return true;
}

void BaseApp::attach(runtime::TickScheduler& scheduler) {
    if (runtime_) {
        runtime_->attach(scheduler);
    }
}

void BaseApp::detach(runtime::TickScheduler& scheduler) {
    if (runtime_) {
        runtime_->detach(scheduler);
    }
}

void BaseApp::tick() {
    acceptClientConnections();
    for (auto& session : clientSessions_) {
        session->pump();
    }
    cleanupClients();
}

BaseRuntime& BaseApp::runtime() {
    return *runtime_;
}

const BaseRuntime& BaseApp::runtime() const {
    return *runtime_;
}

EntityDefRegistry& BaseApp::registry() {
    return registry_;
}

const EntityDefRegistry& BaseApp::registry() const {
    return registry_;
}

runtime::Entity* BaseApp::createEntity(const std::string& entityType) {
    return runtime_ ? runtime_->createEntity(entityType) : nullptr;
}

runtime::Entity* BaseApp::findEntity(runtime::EntityId id) const {
    return runtime_ ? runtime_->findEntity(id) : nullptr;
}

bool BaseApp::destroyEntity(runtime::EntityId id) {
    return runtime_ ? runtime_->destroyEntity(id) : false;
}

std::size_t BaseApp::restoreEntities(const std::string& entityType) {
    return runtime_ ? runtime_->restoreEntities(entityType) : 0;
}

bool BaseApp::requestCreateCell(runtime::EntityId entityId,
                                 const std::string& entityType,
                                 const runtime::Vector3& position,
                                 runtime::ComponentId targetCellApp) {
    return runtime_ ? runtime_->requestCreateCell(entityId, entityType, position, targetCellApp) : false;
}

bool BaseApp::requestDestroyCell(runtime::EntityId entityId,
                                  runtime::ComponentId targetCellApp) {
    return runtime_ ? runtime_->requestDestroyCell(entityId, targetCellApp) : false;
}

void BaseApp::acceptClientConnections() {
    while (auto conn = clientListener_.accept()) {
        auto session = std::make_unique<login::ClientSession>(conn);
        auto* rawSession = session.get();
        session->setMessageCallback(
            [this, rawSession](login::ClientMessageType type,
                               std::span<const std::byte> payload) {
                onClientMessage(rawSession, type, payload);
            });
        clientSessions_.push_back(std::move(session));
    }
}

void BaseApp::onClientMessage(login::ClientSession* session,
                               login::ClientMessageType type,
                               std::span<const std::byte> payload) {
    switch (type) {
        case login::ClientMessageType::EnterGame: {
            std::string token;
            if (login::ClientProtocol::decodeEnterGame(payload, token)) {
                handleEnterGame(session, token);
            }
            break;
        }
        default:
            break;
    }
}

void BaseApp::handleEnterGame(login::ClientSession* session, const std::string& token) {
    std::string accountId, realmId;
    login::EnterGameResponse resp;

    if (!login::SessionToken::validate(token, accountId, realmId)) {
        resp.success = false;
        resp.error = "invalid token";
        auto data = login::ClientProtocol::encodeEnterGameResponse(resp);
        session->send(std::span<const std::byte>(data.data(), data.size()));
        return;
    }

    // Try to restore existing entity or create new one
    auto* entity = runtime_->createEntity("Avatar");
    if (!entity) {
        resp.success = false;
        resp.error = "failed to create entity";
        auto data = login::ClientProtocol::encodeEnterGameResponse(resp);
        session->send(std::span<const std::byte>(data.data(), data.size()));
        return;
    }

    sessionEntityMap_[session] = entity->id();

    // Request cell entity creation on CellApp (component 2 by default)
    requestCreateCell(entity->id(), entity->entityType(),
                      runtime::Vector3{0.0f, 0.0f, 0.0f},
                      runtime::ComponentId{2});

    resp.success = true;
    resp.entityId = entity->id();
    resp.entityType = entity->entityType();
    auto data = login::ClientProtocol::encodeEnterGameResponse(resp);
    session->send(std::span<const std::byte>(data.data(), data.size()));
}

void BaseApp::cleanupClients() {
    std::erase_if(clientSessions_, [this](const std::unique_ptr<login::ClientSession>& s) {
        if (!s->isConnected()) {
            sessionEntityMap_.erase(s.get());
            return true;
        }
        return false;
    });
}

}  // namespace theseed::core
