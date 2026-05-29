#include "theseed/db/DBApp.h"
#include "theseed/db/DBProtocol.h"
#include "theseed/runtime/NetworkTransport.h"
#include "theseed/runtime/TcpConnection.h"

#include <cstring>
#include <iostream>

namespace theseed::db {

DBApp::DBApp(Config config)
    : config_(std::move(config)) {
    listener_.setConnectionFactory([]() {
        return runtime::TcpConnection::create();
    });
}

DBApp::~DBApp() {
    stop();
}

bool DBApp::init() {
    store_ = std::make_shared<core::FileEntityStore>(config_.storePath);
    hub_ = std::make_shared<runtime::TransportHub>(config_.componentId);

    if (!listener_.listen(config_.listenHost, config_.listenPort)) {
        std::cerr << "DBApp: failed to listen on " << config_.listenHost
                  << ":" << config_.listenPort << std::endl;
        return false;
    }
    return true;
}

void DBApp::tick() {
    acceptConnections();
    hub_->tick();
    processMessages();
}

void DBApp::stop() {
    listener_.close();
    hub_.reset();
}

void DBApp::acceptConnections() {
    while (auto conn = listener_.accept()) {
        auto transport = std::make_shared<runtime::NetworkTransport>(conn);
        // Assign sequential component IDs to connected BaseApps
        static runtime::ComponentId nextPeer{1};
        auto peerId = nextPeer++;
        hub_->connectPeer(peerId, transport);
    }
}

void DBApp::processMessages() {
    runtime::RuntimeInvocation inv;
    while (hub_->receive(config_.componentId, &inv, 1) > 0) {
        handleInvocation(inv);
    }
}

void DBApp::handleInvocation(runtime::RuntimeInvocation& inv) {
    const auto& method = inv.method;

    if (method == DBMethod::kLoad) {
        handleLoad(inv);
    } else if (method == DBMethod::kSave) {
        handleSave(inv);
    } else if (method == DBMethod::kRemove) {
        handleRemove(inv);
    } else if (method == DBMethod::kAllocId) {
        handleAllocId(inv);
    } else if (method == DBMethod::kListIds) {
        handleListIds(inv);
    } else if (method == DBMethod::kListTypes) {
        handleListTypes(inv);
    }
}

void DBApp::handleLoad(const runtime::RuntimeInvocation& inv) {
    core::EntityId id;
    std::string entityType;
    if (!DBProtocol::decodeLoadRequest(inv.payload, id, entityType)) {
        auto resp = DBProtocol::encodeLoadResponse(false, {});
        sendResponse(inv.sourceComponent, DBMethod::kLoadOk,
                     std::span<const std::byte>(resp.data(), resp.size()));
        return;
    }

    core::EntityData data;
    bool ok = store_->load(id, entityType, data);
    auto resp = DBProtocol::encodeLoadResponse(ok, data);
    sendResponse(inv.sourceComponent, DBMethod::kLoadOk,
                 std::span<const std::byte>(resp.data(), resp.size()));
}

void DBApp::handleSave(const runtime::RuntimeInvocation& inv) {
    core::EntityId id;
    core::EntityData data;
    if (!DBProtocol::decodeSaveRequest(inv.payload, id, data)) {
        auto resp = DBProtocol::encodeSaveResponse(false);
        sendResponse(inv.sourceComponent, DBMethod::kSaveOk,
                     std::span<const std::byte>(resp.data(), resp.size()));
        return;
    }

    bool ok = store_->save(id, data);
    auto resp = DBProtocol::encodeSaveResponse(ok);
    sendResponse(inv.sourceComponent, DBMethod::kSaveOk,
                 std::span<const std::byte>(resp.data(), resp.size()));
}

void DBApp::handleRemove(const runtime::RuntimeInvocation& inv) {
    core::EntityId id;
    if (!DBProtocol::decodeRemoveRequest(inv.payload, id)) {
        auto resp = DBProtocol::encodeRemoveResponse(false);
        sendResponse(inv.sourceComponent, DBMethod::kRemoveOk,
                     std::span<const std::byte>(resp.data(), resp.size()));
        return;
    }

    bool ok = store_->remove(id);
    auto resp = DBProtocol::encodeRemoveResponse(ok);
    sendResponse(inv.sourceComponent, DBMethod::kRemoveOk,
                 std::span<const std::byte>(resp.data(), resp.size()));
}

void DBApp::handleAllocId(const runtime::RuntimeInvocation& inv) {
    auto id = store_->allocId();
    auto resp = DBProtocol::encodeAllocIdResponse(id);
    sendResponse(inv.sourceComponent, DBMethod::kAllocIdOk,
                 std::span<const std::byte>(resp.data(), resp.size()));
}

void DBApp::handleListIds(const runtime::RuntimeInvocation& inv) {
    std::string entityType;
    auto payload = inv.payload;
    // Decode entityType from payload
    if (payload.size() < 4) return;
    std::size_t offset = 0;
    auto len = static_cast<std::uint32_t>(payload[0])
             | (static_cast<std::uint32_t>(payload[1]) << 8)
             | (static_cast<std::uint32_t>(payload[2]) << 16)
             | (static_cast<std::uint32_t>(payload[3]) << 24);
    offset = 4;
    if (offset + len <= payload.size()) {
        entityType.resize(len);
        if (len > 0) std::memcpy(entityType.data(), payload.data() + offset, len);
    }

    auto ids = store_->listIdsByType(entityType);
    auto resp = DBProtocol::encodeListIdsResponse(ids);
    sendResponse(inv.sourceComponent, DBMethod::kListIdsOk,
                 std::span<const std::byte>(resp.data(), resp.size()));
}

void DBApp::handleListTypes(const runtime::RuntimeInvocation& inv) {
    auto types = store_->listEntityTypes();
    auto resp = DBProtocol::encodeListTypesResponse(types);
    sendResponse(inv.sourceComponent, DBMethod::kListTypesOk,
                 std::span<const std::byte>(resp.data(), resp.size()));
}

void DBApp::sendResponse(runtime::ComponentId target,
                          const std::string& method,
                          std::span<const std::byte> payload) {
    runtime::RuntimeInvocation resp;
    resp.sourceComponent = config_.componentId;
    resp.targetComponent = target;
    resp.entityId = 0;
    resp.method = method;
    resp.payload = std::vector<std::byte>(payload.begin(), payload.end());
    hub_->send(std::move(resp));
    hub_->flush();
}

}  // namespace theseed::db
