#pragma once

#include "theseed/control/machine/MachineAgent.h"

#include <string>

namespace theseed::control::machine {

std::string formatSnapshotText(const NodeSummary& summary);
std::string formatSnapshotJson(const NodeSummary& summary);

// JSON 字符串转义（\\、\"、\n、\r、\t）：快照与审计等 JSON 输出共用。
// 远端可控字符串必须先经此转义再拼入 JSON。
std::string escapeJsonString(const std::string& input);

}  // namespace theseed::control::machine
