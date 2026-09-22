#include <chrono>
#include <cstddef>
#include <thread>
#include <utility>
#include "theseed/db/RemoteEntityStore.h"
#include "theseed/db/DBProtocol.h"

namespace theseed::db {

RemoteEntityStore::RemoteEntityStore(std::shared_ptr<runtime::IRuntimeTransport> transport,
                                     runtime::ComponentId dbComponentId,
                                     runtime::ComponentId localComponentId,
                                     std::chrono::milliseconds requestTimeout)
    : transport_(std::move(transport)),
      dbComponentId_(dbComponentId),
      localComponentId_(localComponentId),
      requestTimeout_(requestTimeout) {}

runtime::RuntimeInvocation RemoteEntityStore::request(
    const std::string& method, std::span<const std::byte> payload) {
    runtime::RuntimeInvocation inv;
    inv.sourceComponent = localComponentId_;
    inv.targetComponent = dbComponentId_;
    inv.entityId = 0;
    inv.method = method;
    inv.payload = std::vector<std::byte>(payload.begin(), payload.end());

    if (transport_->send(std::move(inv)) != runtime::SendResult::Accepted) {
        return {};
    }
    transport_->flush();

    // Pump until we get a response, bounded by requestTimeout_ so a dead
    // DBApp degrades to a failed request instead of a busy-wait hang.
    const auto deadline = runtime::Clock::now() + requestTimeout_;
    const auto expect = std::string(method) + ".ok";
    runtime::RuntimeInvocation resp;
    while (runtime::Clock::now() < deadline) {
        if (pumpFn_) pumpFn_();
        if (transport_->receive(localComponentId_, &resp, 1) > 0) {
            if (resp.method == expect) return resp;
            continue;  // 杂散消息：丢弃继续等
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return {};
}

void RemoteEntityStore::setPumpFunction(PumpFn pumpFn) {
    pumpFn_ = std::move(pumpFn);
}

bool RemoteEntityStore::load(core::EntityId id, const std::string& entityType,
                              core::EntityData& out) {
    auto req = DBProtocol::encodeLoadRequest(id, entityType);
    auto resp = request(DBMethod::kLoad,
                        std::span<const std::byte>(req.data(), req.size()));
    if (resp.method != DBMethod::kLoadOk) return false;

    bool success;
    if (!DBProtocol::decodeLoadResponse(
            std::span<const std::byte>(resp.payload.data(), resp.payload.size()),
            success, out))
        return false;
    return success;
}

bool RemoteEntityStore::save(core::EntityId id, const core::EntityData& data) {
    auto req = DBProtocol::encodeSaveRequest(id, data);
    auto resp = request(DBMethod::kSave,
                        std::span<const std::byte>(req.data(), req.size()));
    if (resp.method != DBMethod::kSaveOk) return false;

    bool success;
    if (!DBProtocol::decodeSaveResponse(
            std::span<const std::byte>(resp.payload.data(), resp.payload.size()),
            success))
        return false;
    return success;
}

bool RemoteEntityStore::remove(core::EntityId id) {
    auto req = DBProtocol::encodeRemoveRequest(id);
    auto resp = request(DBMethod::kRemove,
                        std::span<const std::byte>(req.data(), req.size()));
    if (resp.method != DBMethod::kRemoveOk) return false;

    bool success;
    if (!DBProtocol::decodeRemoveResponse(
            std::span<const std::byte>(resp.payload.data(), resp.payload.size()),
            success))
        return false;
    return success;
}

core::EntityId RemoteEntityStore::allocId() {
    auto resp = request(DBMethod::kAllocId, {});
    if (resp.method != DBMethod::kAllocIdOk) return 0;

    core::EntityId id;
    if (!DBProtocol::decodeAllocIdResponse(
            std::span<const std::byte>(resp.payload.data(), resp.payload.size()),
            id))
        return 0;
    return id;
}

std::vector<core::EntityId> RemoteEntityStore::listIdsByType(const std::string& entityType) {
    auto req = DBProtocol::encodeListIdsRequest(entityType);
    auto resp = request(DBMethod::kListIds,
                        std::span<const std::byte>(req.data(), req.size()));
    if (resp.method != DBMethod::kListIdsOk) return {};

    std::vector<core::EntityId> ids;
    DBProtocol::decodeListIdsResponse(
        std::span<const std::byte>(resp.payload.data(), resp.payload.size()),
        ids);
    return ids;
}

std::vector<std::string> RemoteEntityStore::listEntityTypes() {
    auto resp = request(DBMethod::kListTypes, {});
    if (resp.method != DBMethod::kListTypesOk) return {};

    std::vector<std::string> types;
    DBProtocol::decodeListTypesResponse(
        std::span<const std::byte>(resp.payload.data(), resp.payload.size()),
        types);
    return types;
}

}  // namespace theseed::db
