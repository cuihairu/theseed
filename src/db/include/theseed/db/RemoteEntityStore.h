#pragma once

#include "theseed/core/IEntityStore.h"
#include "theseed/runtime/RuntimeTransport.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace theseed::db {

class RemoteEntityStore final : public core::IEntityStore {
public:
    RemoteEntityStore(std::shared_ptr<runtime::IRuntimeTransport> transport,
                      runtime::ComponentId dbComponentId,
                      runtime::ComponentId localComponentId);

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
};

}  // namespace theseed::db
