#include "theseed/core/BaseApp.h"
#include "theseed/foundation/Metrics.h"
#include "theseed/login/SessionToken.h"
#include "theseed/runtime/TcpConnection.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <utility>

namespace theseed::core {

BaseApp::ClientPump::ClientPump(BaseApp& owner) : owner_(&owner) {}

void BaseApp::ClientPump::tick(runtime::TickContext& context) {
    static_cast<void>(context);
    owner_->tick();
}

BaseApp::BaseApp(Config config,
                 std::shared_ptr<runtime::IRuntimeTransport> transport,
                 std::shared_ptr<IEntityStore> store)
    : config_(std::move(config)),
      transport_(std::move(transport)),
      store_(std::move(store)),
      clientPump_(*this) {
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
        if (factory) {  // LCOV_EXCL_BR_LINE 假臂不可达：createFactory 对已注册类型恒返回非空工厂 lambda，遍历集合即 defs_ 的键集
            runtime_->registerEntityFactory(entityType, std::move(factory));
        }
    }

    runtime_->setAutoSaveInterval(config_.autoSaveInterval);

    runtime_->setOnAoIEnter([this](runtime::EntityId observerId, runtime::EntityId targetId,
                                     const std::string& targetType, bool hasPosition,
                                     const runtime::Vector3& position) {
        onAoIEnter(observerId, targetId, targetType, hasPosition, position);
    });
    runtime_->setOnAoILeave([this](runtime::EntityId observerId, runtime::EntityId targetId) {
        onAoILeave(observerId, targetId);
    });

    runtime_->setOnClientEvent([this](runtime::EntityId entityId, const std::string& eventName,
                                        std::span<const std::byte> data) {
        onClientEvent(entityId, eventName, data);
    });

    runtime_->setOnWitnessSync([this](runtime::EntityId observerId, runtime::EntityId targetEntityId,
                                        std::span<const std::byte> propertyData,
                                        bool hasPosition, const runtime::Vector3& position) {
        onWitnessPropertySync(observerId, targetEntityId, propertyData, hasPosition, position);
    });

    runtime_->setOnSpaceChange([this](runtime::EntityId entityId, runtime::SpaceId spaceId,
                                        const runtime::Vector3& position) {
        onSpaceChanged(entityId, spaceId, position);
    });

    runtime_->setOnEntityDestroyed([this](runtime::EntityId entityId,
                                            const std::string& entityType) {
        onEntityDestroyed(entityId, entityType);
    });

    for (const auto& entityType : store_->listEntityTypes()) {
        if (runtime_->findEntitiesByType(entityType).empty()) {  // LCOV_EXCL_BR_LINE 假臂不可达：init 阶段 runtime 新建且按类型逐个恢复，查询时该类型恒为空
            runtime_->restoreEntities(entityType);
        }
    }

    clientListener_.listen(config_.clientListenHost, config_.clientListenPort);

    if (config_.ops.enabled) {
        ops::ProcessInfo info{};  // LCOV_EXCL_BR_LINE 聚合初始化字符串成员构造的 SSO/堆库内联分支，字面常量恒短串，非业务分支
        info.role = "BaseApp";
        info.version = "0.1.0";
        info.startTime = std::chrono::system_clock::now();
        info.componentId = config_.componentId;

        opsInspector_ = std::make_unique<ops::OpsInspector>(std::move(info), [this] {
            ops::RuntimeInfo rt{};
            if (runtime_) {  // LCOV_EXCL_BR_LINE init 建 inspector 前已 make_unique runtime_，空臂不可达
                rt.entityCount = runtime_->entityCount();
            }
            rt.entityTypes = registry_.entityTypes();
            if (transport_) {  // LCOV_EXCL_BR_LINE 构造函数拒绝空 transport，空臂不可达
                rt.transportStats = transport_->stats();
            }
            return rt;
        });

        ops::OpsServer::Config opsCfg{};  // LCOV_EXCL_BR_LINE 聚合初始化字符串成员构造的库内联分支，非业务分支
        opsCfg.host = config_.ops.host;
        opsCfg.port = config_.ops.port;
        opsCfg.maxConnections = config_.ops.maxConnections;
        opsServer_ = std::make_unique<ops::OpsServer>(opsCfg, *opsInspector_);
        opsServer_->start();
    }

    return true;
}

void BaseApp::attach(runtime::TickScheduler& scheduler) {
    if (runtime_) {
        runtime_->attach(scheduler);
    }
    scheduler.registerTickable(runtime::TickPhase::Flush, clientPump_);
}

void BaseApp::detach(runtime::TickScheduler& scheduler) {
    if (runtime_) {
        runtime_->detach(scheduler);
    }
    static_cast<void>(scheduler.unregisterTickable(runtime::TickPhase::Flush, clientPump_));
}

void BaseApp::tick() {
    acceptClientConnections();
    for (auto& session : clientSessions_) {
        session->pump();
    }
    flushClientPropertyUpdates();
    cleanupClients();

    if (transport_) {  // LCOV_EXCL_BR_LINE 构造函数拒绝空 transport，空臂不可达
        transportStatsCollector_.collect(transport_->stats());
    }

    if (opsServer_) {
        opsServer_->tick();
    }
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

std::uint16_t BaseApp::clientListenPort() const {
    return clientListener_.localPort();
}

std::uint16_t BaseApp::opsListenPort() const {
    return opsServer_ ? opsServer_->localPort() : 0;
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
                                 runtime::ComponentId targetCellApp,
                                 runtime::SpaceId spaceId) {
    return runtime_ ? runtime_->requestCreateCell(entityId, entityType, position, targetCellApp, spaceId) : false;
}

bool BaseApp::requestDestroyCell(runtime::EntityId entityId,
                                  runtime::ComponentId targetCellApp) {
    return runtime_ ? runtime_->requestDestroyCell(entityId, targetCellApp) : false;
}

bool BaseApp::requestTeleport(runtime::EntityId entityId,
                               runtime::SpaceId targetSpaceId,
                               const runtime::Vector3& position) {
    return runtime_ ? runtime_->requestTeleport(entityId, targetSpaceId, position) : false;
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
        case login::ClientMessageType::Action: {
            login::ActionMsg msg;
            if (login::ClientProtocol::decodeAction(payload, msg)) {
                handleClientAction(msg);
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
    entitySessionMap_[entity->id()] = session;

    // Request cell entity creation on CellApp (component 2 by default)
    requestCreateCell(entity->id(), entity->entityType(),
                      runtime::Vector3{0.0f, 0.0f, 0.0f},
                      runtime::ComponentId{2});

    resp.success = true;
    resp.entityId = entity->id();
    resp.entityType = entity->entityType();
    auto data = login::ClientProtocol::encodeEnterGameResponse(resp);
    session->send(std::span<const std::byte>(data.data(), data.size()));

    // Send initial property snapshot
    auto snapshot = entity->buildFullPropertySnapshot(runtime::PropertyFlag::Base);
    if (!snapshot.empty()) {
        auto encoded = runtime::PropertyReplication::encodeDelta(snapshot);
        login::PropertySyncMsg syncMsg;
        syncMsg.entityId = entity->id();
        syncMsg.propertyData = std::move(encoded);
        auto syncData = login::ClientProtocol::encodePropertySync(syncMsg);
        session->send(std::span<const std::byte>(syncData.data(), syncData.size()));
    }
    entity->clearClientDirtyFlags();
}

void BaseApp::cleanupClients() {
    std::erase_if(clientSessions_, [this](const std::unique_ptr<login::ClientSession>& s) {
        if (!s->isConnected()) {
            auto it = sessionEntityMap_.find(s.get());
            if (it != sessionEntityMap_.end()) {
                auto entityId = it->second;

                auto* entity = runtime_->findEntity(entityId);
                if (entity) {
                    auto* cellCall = entity->cellEntityCall();
                    if (cellCall && cellCall->isValid()) {  // LCOV_EXCL_BR_LINE isValid 假臂不可构造：cellCall 非空即 valid（bind 恒携带 target），null 假臂已由无绑定销毁场景覆盖
                        runtime_->requestDestroyCell(entityId, cellCall->targetComponent());
                    }
                    runtime_->destroyEntity(entityId);
                }

                entitySessionMap_.erase(entityId);
            }
            sessionEntityMap_.erase(s.get());
            return true;
        }
        return false;
    });
}

void BaseApp::flushClientPropertyUpdates() {
    for (auto& [session, entityId] : sessionEntityMap_) {
        if (!session->isConnected()) continue;

        auto* entity = runtime_->findEntity(entityId);
        if (!entity || !entity->isActive()) continue;

        auto deltas = entity->buildClientDirtyPropertyDelta(runtime::PropertyFlag::Base);
        if (deltas.empty()) continue;

        auto encoded = runtime::PropertyReplication::encodeDelta(deltas);
        login::PropertySyncMsg msg;
        msg.entityId = entityId;
        msg.propertyData = std::move(encoded);
        auto data = login::ClientProtocol::encodePropertySync(msg);
        session->send(std::span<const std::byte>(data.data(), data.size()));

        entity->clearClientDirtyFlags();
    }
}

void BaseApp::handleClientAction(const login::ActionMsg& msg) {
    auto* entity = runtime_->findEntity(msg.entityId);
    if (!entity || !entity->isActive()) return;

    // Validate action against EntityDef
    auto& def = entity->propertyBlock().def();
    const auto* methodDesc = def.findMethod(msg.actionName);
    if (!methodDesc) return;

    switch (methodDesc->side) {
        case runtime::MethodSide::Base: {
            // Dispatch directly on base entity
            entity->dispatchMethod(msg.actionName,
                std::span<const std::byte>(msg.actionData.data(), msg.actionData.size()));
            break;
        }
        case runtime::MethodSide::Cell: {
            // Forward to CellApp as a typed method call
            auto* cellCall = entity->cellEntityCall();
            if (cellCall && cellCall->isValid()) {  // LCOV_EXCL_BR_LINE isValid 假臂不可构造：cellCall 非空即 valid，null 假臂已由无绑定 action 场景覆盖
                entity->callCell(msg.actionName,
                    std::span<const std::byte>(msg.actionData.data(), msg.actionData.size()));
            }
            break;
        }
        default:
            break;
    }
}

login::ClientSession* BaseApp::findSessionByEntity(runtime::EntityId entityId) const {
    auto it = entitySessionMap_.find(entityId);
    if (it == entitySessionMap_.end()) return nullptr;
    return it->second;
}

void BaseApp::bindSessionToEntity(login::ClientSession* session, runtime::EntityId entityId) {
    sessionEntityMap_[session] = entityId;
    entitySessionMap_[entityId] = session;
}

void BaseApp::takeClientSession(std::unique_ptr<login::ClientSession> session) {
    clientSessions_.push_back(std::move(session));
}

void BaseApp::onAoIEnter(runtime::EntityId observerId, runtime::EntityId targetId,
                           const std::string& targetType, bool hasPosition,
                           const runtime::Vector3& position) {
    auto* session = findSessionByEntity(observerId);
    if (!session || !session->isConnected()) return;

    login::EntityEnterMsg msg;
    msg.entityId = targetId;
    msg.entityType = targetType;
    if (hasPosition) {
        msg.posX = position.x;
        msg.posY = position.y;
        msg.posZ = position.z;
    }
    auto data = login::ClientProtocol::encodeEntityEnter(msg);
    session->send(std::span<const std::byte>(data.data(), data.size()));
}

void BaseApp::onAoILeave(runtime::EntityId observerId, runtime::EntityId targetId) {
    auto* session = findSessionByEntity(observerId);
    if (!session || !session->isConnected()) return;

    login::EntityLeaveMsg msg;
    msg.entityId = targetId;
    auto data = login::ClientProtocol::encodeEntityLeave(msg);
    session->send(std::span<const std::byte>(data.data(), data.size()));
}

void BaseApp::onClientEvent(runtime::EntityId entityId, const std::string& eventName,
                              std::span<const std::byte> data) {
    auto* session = findSessionByEntity(entityId);
    if (!session || !session->isConnected()) return;

    login::EntityEventMsg msg;
    msg.entityId = entityId;
    msg.eventName = eventName;
    msg.eventData.assign(data.begin(), data.end());
    auto encoded = login::ClientProtocol::encodeEntityEvent(msg);
    session->send(std::span<const std::byte>(encoded.data(), encoded.size()));
}

void BaseApp::onWitnessPropertySync(runtime::EntityId observerId, runtime::EntityId targetEntityId,
                                      std::span<const std::byte> propertyData,
                                      bool hasPosition, const runtime::Vector3& position) {
    auto* session = findSessionByEntity(observerId);
    if (!session || !session->isConnected()) return;

    login::PropertySyncMsg msg;
    msg.entityId = targetEntityId;
    msg.propertyData.assign(propertyData.begin(), propertyData.end());
    msg.hasPosition = hasPosition;
    if (hasPosition) {
        msg.posX = position.x;
        msg.posY = position.y;
        msg.posZ = position.z;
    }
    auto encoded = login::ClientProtocol::encodePropertySync(msg);
    session->send(std::span<const std::byte>(encoded.data(), encoded.size()));
}

void BaseApp::onSpaceChanged(runtime::EntityId entityId, runtime::SpaceId spaceId,
                               const runtime::Vector3& position) {
    auto* session = findSessionByEntity(entityId);
    if (!session || !session->isConnected()) return;

    login::SpaceChangeMsg msg;
    msg.entityId = entityId;
    msg.spaceId = spaceId;
    msg.posX = position.x;
    msg.posY = position.y;
    msg.posZ = position.z;
    auto encoded = login::ClientProtocol::encodeSpaceChange(msg);
    session->send(std::span<const std::byte>(encoded.data(), encoded.size()));
}

void BaseApp::onEntityDestroyed(runtime::EntityId entityId, const std::string& entityType) {
    // Notify client
    auto* session = findSessionByEntity(entityId);
    if (session && session->isConnected()) {
        login::EntityLeaveMsg msg;
        msg.entityId = entityId;
        auto data = login::ClientProtocol::encodeEntityLeave(msg);
        session->send(std::span<const std::byte>(data.data(), data.size()));
    }

    // Clean up session mapping
    auto sessionIt = entitySessionMap_.find(entityId);
    if (sessionIt != entitySessionMap_.end()) {
        sessionEntityMap_.erase(sessionIt->second);
        entitySessionMap_.erase(sessionIt);
    }
}

}  // namespace theseed::core
