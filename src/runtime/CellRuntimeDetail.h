#pragma once

#include "theseed/runtime/RuntimeTypes.h"

#include <cstddef>
#include <vector>

namespace theseed::runtime::detail {

// entity.cellReady payload: entityId(8) + cellComponentId(4) = 12
std::vector<std::byte> encodeCellReady(EntityId entityId, ComponentId cellComponentId);

}  // namespace theseed::runtime::detail
