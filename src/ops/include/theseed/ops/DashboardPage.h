#pragma once

#include <string_view>

namespace theseed::ops {

// OpsServer 内嵌 dashboard 的单页 HTML（自包含 HTML+CSS+JS，无外部依赖）。
// 前端 fetch /health /inspect /entities /metrics 轮询渲染：在线玩家、
// 实体数、tick 耗时、内存、队列深度、最近告警（由指标增量推导）。
std::string_view dashboardHtml();

}  // namespace theseed::ops
