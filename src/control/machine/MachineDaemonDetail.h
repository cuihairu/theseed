#pragma once

// MachineDaemon 实现内部共用的辅助件（不对上层公开）。MachineDaemon 的
// 各 handler 组按面拆成独立 TU（process / session / diagnostic / config），
// 它们共用的载荷解析、遥测命名与拒绝出口在此声明，定义集中在
// MachineDaemonDetail.cpp——单一定义，避免头文件内联造成重复符号与
// 覆盖归因漂移。

#include "theseed/control/machine/AuditEntry.h"
#include "theseed/foundation/Metrics.h"
#include "theseed/foundation/Tracing.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace theseed::control::machine::detail {

// 控制面遥测命名（05-telemetry MVP：结构化 logs + 基础 metrics + 关键
// traces 的控制面切片）。指标名进程级单例、snake_case + _count 后缀，
// 与 LoginApp/DBApp 同族口径。
inline constexpr const char* kSnapshotCount = "machine_snapshot_count";
inline constexpr const char* kAuditCount = "machine_audit_count";
inline constexpr const char* kExecuteAcceptedCount =
    "machine_execute_accepted_count";
inline constexpr const char* kExecuteRejectedCount =
    "machine_execute_rejected_count";
inline constexpr const char* kUnknownMethodCount = "machine_unknown_method_count";
inline constexpr const char* kExecuteDurationMs = "machine_execute_duration_ms";
// 治理切片（05 遥测同族口径）：枚举只读与处置各一对接受/拒绝计数。
inline constexpr const char* kProcessListCount = "machine_process_list_count";
inline constexpr const char* kProcessListRejectedCount =
    "machine_process_list_rejected_count";
inline constexpr const char* kTerminateAcceptedCount =
    "machine_terminate_accepted_count";
inline constexpr const char* kTerminateRejectedCount =
    "machine_terminate_rejected_count";
// 审计命令口径：治理动作与 execute 审计同族（command 串入审计条目，
// 中心侧按命令名可辨动作类型）。
inline constexpr const char* kGovernListCommand = "process.list";
inline constexpr const char* kGovernKillCommand = "process.kill";
// 诊断采样入口（04 §7）：触发（写侧，消耗性能预算）与访问（读侧，
// 明细外泄面）各一对接受/拒绝计数。
inline constexpr const char* kProfileTriggerAcceptedCount =
    "machine_profile_trigger_accepted_count";
inline constexpr const char* kProfileTriggerRejectedCount =
    "machine_profile_trigger_rejected_count";
inline constexpr const char* kProfileAccessAcceptedCount =
    "machine_profile_access_accepted_count";
inline constexpr const char* kProfileAccessRejectedCount =
    "machine_profile_access_rejected_count";
// 审计命令口径同治理：触发/清单/下载三命令名入审计条目。
inline constexpr const char* kProfileTriggerCommand = "profiler.trigger";
inline constexpr const char* kProfileListCommand = "profiler.list";
inline constexpr const char* kProfileDownloadCommand = "profiler.download";
// §6.1 角色门落到只读面后，快照/审计查询的拒绝也要有同款计数（此前
// 只读面无拒绝路径，计数器是单数；拒绝臂补齐成对口径）。
inline constexpr const char* kSnapshotRejectedCount =
    "machine_snapshot_rejected_count";
inline constexpr const char* kAuditRejectedCount = "machine_audit_rejected_count";
// §6.3 运行时配置热改：Admin 级动作一对接受/拒绝计数。
inline constexpr const char* kConfigApplyAcceptedCount =
    "machine_config_apply_accepted_count";
inline constexpr const char* kConfigApplyRejectedCount =
    "machine_config_apply_rejected_count";
// 只读面拒绝也入审计（拒绝是动作，照记不漏）；命令名与 RPC 方法同名。
inline constexpr const char* kSnapshotCommand = "machine.snapshot";
inline constexpr const char* kAuditQueryCommand = "machine.audit";
inline constexpr const char* kConfigApplyCommand = "config.apply";
// §6.1 受控命令（kick/draining/shutdown）各一对接受/拒绝计数，同族口径。
inline constexpr const char* kKickAcceptedCount = "machine_kick_accepted_count";
inline constexpr const char* kKickRejectedCount = "machine_kick_rejected_count";
inline constexpr const char* kDrainAcceptedCount = "machine_drain_accepted_count";
inline constexpr const char* kDrainRejectedCount = "machine_drain_rejected_count";
inline constexpr const char* kShutdownAcceptedCount =
    "machine_shutdown_accepted_count";
inline constexpr const char* kShutdownRejectedCount =
    "machine_shutdown_rejected_count";
// 审计命令口径：§6.1 命令族按动作命名（与 process.kill / config.apply
// 同风格），来源组件即 §6.2 operatorId。
inline constexpr const char* kKickCommand = "session.kick";
inline constexpr const char* kDrainCommand = "node.drain";
inline constexpr const char* kShutdownCommand = "node.shutdown";
// §8 Phase 2 会话运维面：list/kick-sessions 各一对接受/拒绝计数（与
// §6.1 命令族同款配对口径；单数 machine_kick_* 已被单令牌 kick 占用，
// 批量面用复数 sessions 免歧义）。
inline constexpr const char* kListSessionsAcceptedCount =
    "machine_list_sessions_accepted_count";
inline constexpr const char* kListSessionsRejectedCount =
    "machine_list_sessions_rejected_count";
inline constexpr const char* kKickSessionsAcceptedCount =
    "machine_kick_sessions_accepted_count";
inline constexpr const char* kKickSessionsRejectedCount =
    "machine_kick_sessions_rejected_count";
inline constexpr const char* kListSessionsCommand = "sessions.list";
inline constexpr const char* kKickSessionsCommand = "sessions.kick";
// §8 Phase 2 续期（extend-sessions）一对计数 + 联动通知送达计数
//（通知无接受/拒绝语义——best-effort，只计送达）。
inline constexpr const char* kExtendSessionsAcceptedCount =
    "machine_extend_sessions_accepted_count";
inline constexpr const char* kExtendSessionsRejectedCount =
    "machine_extend_sessions_rejected_count";
inline constexpr const char* kSessionNotifyCount = "machine_session_notify_count";
inline constexpr const char* kExtendSessionsCommand = "sessions.extend";

// §6.3 禁改四类（协议定义 / 持久化 schema / entity property flags /
// 迁移语义）：键前缀 → 类别名。命中即拒绝且指认类别——在线修改会破坏
// 滚动升级 / 存量数据兼容 / 迁移正确性，不属于运维热改的授权范围。
struct ProtectedConfigClass final {
    const char* prefix;
    const char* className;
};
inline constexpr ProtectedConfigClass kProtectedConfigClasses[] = {
    {"protocol.", "protocol definition"},
    {"persistence.", "persistence schema"},
    {"entity.property", "entity property flags"},
    {"migration.", "migration semantics"},
};

// §6.3 白名单：允许在线生效的配置键。白名单是热改的唯一通道：不在表内
// 的键一律拒绝（含禁改四类之外的任意新键——扩面须改码评审，不允许配置
// 自身把门打开）。
inline constexpr const char* kConfigReportIntervalKey = "ops.report_interval_ms";

std::vector<std::byte> toBytes(const std::string& text);

// 审计条目 → JSON 对象（字段顺序固定，时间 epoch 毫秒）。
std::string auditEntryJson(const AuditEntry& entry);

foundation::Counter& telemetryCounter(const char* name);

// 控制面动作拒绝（治理/诊断采样共用）：计数 + span 标记 + 结构化日志。
// span 可空（只读动作不进 trace）；审计与错误响应由调用方处理。
void rejectControlAction(const AuditEntry& entry,
                         foundation::SpanScope* span,
                         const char* counterName,
                         const std::string& logMessage,
                         const std::string& reason);

// machine.terminate 请求载荷 = pid 十进制串；pid 0 无意义（不是合法治理
// 目标），解析失败一律 ok=false。
struct ParsedPid {
    bool ok = false;
    std::uint32_t value = 0;
};

ParsedPid parsePidPayload(const std::vector<std::byte>& payload);

// 诊断产物句柄载荷解析：与 pid 同为十进制整串，但句柄是 uint64——
// 单独成解析体，不复用不硬转。0 是"无产物"哨兵，不作为查询目标。
struct ParsedHandle {
    bool ok = false;
    std::uint64_t value = 0;
};

ParsedHandle parseHandlePayload(const std::vector<std::byte>& payload);

// machine.config.apply 请求载荷 = key '\0' value（与 execute 同一 NUL
// 分隔约定）；key 不得为空。value 的合法性由白名单命中后的解析负责。
struct ParsedConfigPayload {
    bool ok = false;
    std::string key;
    std::string value;
};

ParsedConfigPayload parseConfigPayload(const std::vector<std::byte>& payload);

// 白名单键的值解析：毫秒数（十进制、全串消费）。0 合法 = 关闭该可调项
// （与 reportInterval 的 0 = 关闭口径一致）。
struct ParsedMillis {
    bool ok = false;
    std::uint64_t value = 0;
};

ParsedMillis parseMillisValue(const std::string& text);

// kick 审计的令牌指纹：会话令牌是登录凭证，原文不入审计环（接受侧与
// 拒绝侧同口径）；长度足以核对异常与滥用，不留下可复用的残迹。
std::string redactSessionToken(const std::string& token);

// 批量 kick 的作用域选择器（§8 Phase 2）：指纹不可逆（令牌原文不出
// 存储/进程），批量处置按运维可见属性圈选——all | account=<id> |
// realm=<id>。空值后缀（如 "account="）不成立，走畸形臂。
struct SessionScope final {
    bool valid = false;
    bool all = false;
    bool byAccount = false;  // valid 且非 all 时二选一
    std::string value;
};

SessionScope parseSessionScope(const std::string& text);

}  // namespace theseed::control::machine::detail
