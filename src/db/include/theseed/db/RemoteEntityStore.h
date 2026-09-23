#pragma once

#include "theseed/core/IEntityStore.h"
#include "theseed/runtime/RuntimeTransport.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace theseed::db {

class RemoteEntityStore final : public core::IEntityStore {
public:
    using PumpFn = std::function<void()>;

    // requestTimeout：等待 DBApp 应答的上限，超时/发送失败返回 method 为空的
    // RuntimeInvocation（各调用方的 method 校验自然判 false），避免 DBApp 失联时忙等挂死。
    RemoteEntityStore(std::shared_ptr<runtime::IRuntimeTransport> transport,
                      runtime::ComponentId dbComponentId,
                      runtime::ComponentId localComponentId,
                      std::chrono::milliseconds requestTimeout = std::chrono::milliseconds{5000});

    void setPumpFunction(PumpFn pumpFn);

    bool load(core::EntityId id, const std::string& entityType,
              core::EntityData& out) override;
    bool save(core::EntityId id, const core::EntityData& data) override;
    bool remove(core::EntityId id) override;
    core::EntityId allocId() override;
    std::vector<core::EntityId> listIdsByType(const std::string& entityType) override;
    std::vector<std::string> listEntityTypes() override;

private:
    runtime::RuntimeInvocation request(const std::string& method,
                                        std::span<const std::byte> payload);

    std::shared_ptr<runtime::IRuntimeTransport> transport_;
    runtime::ComponentId dbComponentId_;
    runtime::ComponentId localComponentId_;
    std::chrono::milliseconds requestTimeout_;
    PumpFn pumpFn_;
};

}  // namespace theseed::db
