#pragma once

#include "theseed/control/machine/MachineAgent.h"

#include <string>

namespace theseed::control::machine {

std::string formatSnapshotText(const NodeSummary& summary);
std::string formatSnapshotJson(const NodeSummary& summary);

}  // namespace theseed::control::machine
