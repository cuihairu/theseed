#include "CellRuntimeDetail.h"

#include <cstring>

namespace theseed::runtime::detail {

std::vector<std::byte> encodeCellReady(EntityId entityId, ComponentId cellComponentId) {
    std::vector<std::byte> payload(sizeof(EntityId) + sizeof(ComponentId));
    std::memcpy(payload.data(), &entityId, sizeof(entityId));
    std::memcpy(payload.data() + sizeof(entityId), &cellComponentId, sizeof(cellComponentId));
    return payload;
}

}  // namespace theseed::runtime::detail
