#pragma once

#include "theseed/control/machine/AccessControl.h"
#include "theseed/control/machine/AuditEntry.h"
#include "theseed/control/machine/MachineAgent.h"
#include "theseed/control/machine/ProfileRelay.h"
#include "theseed/foundation/SessionStore.h"
#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/TcpListener.h"
#include "theseed/runtime/TickProfiler.h"
#include "theseed/runtime/TransportHub.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace theseed::control::machine {

// 控制面 RPC 方法名（RuntimeInvocation.method）。
namespace MachineMethod {
inline constexpr const char* kSnapshot = "machine.snapshot";
inline constexpr const char* kSnapshotOk = "machine.snapshot.ok";
inline constexpr const char* kExecute = "machine.execute";
inline constexpr const char* kExecuteOk = "machine.execute.ok";
// 审计查询：payload = 最近 auditCapacity 条的 JSON 数组（时间升序）。
inline constexpr const char* kAudit = "machine.audit";
inline constexpr const char* kAuditOk = "machine.audit.ok";
// 主机级非受控进程治理（策略化，见 ProcessGovernPolicy）：
//   machine.processes → machine.processes.ok  payload = 非受控进程 JSON
//                       数组（[{"pid":N,"name":"..."}]，不含受管进程）
//   machine.terminate → machine.terminate.ok  payload = 1 字节 0x01/0x00
//                       请求 payload = 目标 pid 十进制串
inline constexpr const char* kProcesses = "machine.processes";
inline constexpr const char* kProcessesOk = "machine.processes.ok";
inline constexpr const char* kTerminate = "machine.terminate";
inline constexpr const char* kTerminateOk = "machine.terminate.ok";
// 诊断采样入口（04 §7：触发/下载鉴权归 Ops Control Plane，见
// DiagnosticsPolicy）：
//   machine.profile.trigger → machine.profile.trigger.ok
//                       payload = 窗口句柄十进制串（限流拒绝 → kError）
//   machine.profiles     → machine.profiles.ok   payload = 产物清单 JSON
//                       数组（[{"handle":N,"tick_count":N,"window_ms":X}]）
//   machine.profile      → machine.profile.ok    payload = 产物 JSON 快照
//                       请求 payload = 句柄十进制串（未知句柄 → kError）
inline constexpr const char* kProfileTrigger = "machine.profile.trigger";
inline constexpr const char* kProfileTriggerOk = "machine.profile.trigger.ok";
inline constexpr const char* kProfiles = "machine.profiles";
inline constexpr const char* kProfilesOk = "machine.profiles.ok";
inline constexpr const char* kProfile = "machine.profile";
inline constexpr const char* kProfileOk = "machine.profile.ok";
// 运行时配置热改（04 §6.3，Admin 级）：machine.config.apply →
// machine.config.apply.ok  payload = 1 字节 0x01
//                     请求 payload = key '\0' value
//                     （禁改四类命中 / 白名单外 / 角色不足 → kError）
inline constexpr const char* kConfigApply = "machine.config.apply";
inline constexpr const char* kConfigApplyOk = "machine.config.apply.ok";
// §6.1 受控命令（clear temporary bans 仓库无封禁存储未建，见边界）：
//   machine.kick-session → machine.kick-session.ok  payload = 1 字节
//                     0x01（会话令牌已吊销）
//                     请求 payload = 会话令牌原文（未知令牌 → kError；
//                     审计只留长度指纹，令牌原文不进审计环）
//   machine.set-draining → machine.set-draining.ok payload = 1 字节 0x01
//                     请求 payload = 1 字节 0x01/0x00（排水开/关）
//   machine.shutdown → machine.shutdown.ok   payload = 1 字节 0x01
//                     （响应出站后本 tick 收尾时优雅停机：注销中心 +
//                     关听；请求应答先于停机出站）
inline constexpr const char* kKickSession = "machine.kick-session";
inline constexpr const char* kKickSessionOk = "machine.kick-session.ok";
inline constexpr const char* kSetDraining = "machine.set-draining";
inline constexpr const char* kSetDrainingOk = "machine.set-draining.ok";
inline constexpr const char* kShutdown = "machine.shutdown";
inline constexpr const char* kShutdownOk = "machine.shutdown.ok";
// 统一错误响应：payload 为短原因串（可读，供人工诊断与测试断言）。
inline constexpr const char* kError = "machine.error";
}  // namespace MachineMethod

// 受控命令审计条目 AuditEntry 见 machine/AuditEntry.h（daemon 生产与中心
// 聚合共用同一形状，INodeAuditSink 为其上报出口）。

// MachineAgent 的 RPC 输出：把 snapshot/execute/audit 暴露为控制面 TCP 端点。
//
// 协议复用组件间 RuntimeInvocation 帧（与 DBApp 同一族，不另起协议）：
//   machine.snapshot → machine.snapshot.ok  payload = 快照 JSON
//                      （MachineSnapshotCodec::formatSnapshotJson）
//   machine.execute  → machine.execute.ok   payload = 1 字节 0x01/0x00
//                      请求 payload = command '\0' args（命令与参数以
//                      NUL 分隔，与 IMachineAgent::execute 的两参对齐）
//   machine.audit    → machine.audit.ok     payload = 审计条目 JSON 数组
//   machine.processes → machine.processes.ok  payload = 非受控进程 JSON 数组
//                      （来源须过 processGovernPolicy 来源白名单）
//   machine.terminate → machine.terminate.ok  payload = 1 字节 0x01/0x00
//                      （请求 payload = pid 十进制串；来源白名单 + 目标
//                      comm 名单 + 保护类判定：受管进程与自身进程拒绝）
//   其余 method 或畸形 execute 载荷 → machine.error（payload = 原因串）
//
// 每条 execute 尝试与协议层拒绝记入环形审计日志（容量 auditCapacity，
// 满后丢最旧；0 = 关闭审计）。
//
// 服务端连接由对端首条请求的 sourceComponent 自报注册
// （attachServerTransport，与 DBApp 同机制）。调用方负责在自身 tick
// 循环里泵 tick()：accept 新连接 + 收发分发一轮，全部非阻塞；审计日志
// 只在 tick 上下文写入，无锁（单线程假设与 DBApp 一致）。
//
// 中心侧生命周期（reportSink 配置时）：start() 成功监听后用快照
// hostname（nodeId 口径）向中心注册占位；周期上报随后刷新快照；stop()
// 优雅下线立即注销摘除。疑似掉线（无注销机会）由中心 pruneStale 的
// TTL 兜底——注册/注销/掉线摘除三者语义自洽。
//
// 剖面回传接线（04 §7 中心侧）：daemon 以 IProfileMetaSource 身份挂到
// agent（report() 组装 NodeReport 时带上本机剖面清单，经既有上报通道
// 汇聚），并在 tick 里把新固化产物经 artifactSink 推给中心存储——中心
// 侧查询/下载不依赖直连 agent。
//
// 权限分级（04 §6.1）：全部方法先过各自策略白名单（若该面有策略门），
// 再过角色门（Config.roleBindings，见 AccessControl.h）——inspect 面
// （snapshot/audit/清单/剖面下载）需 ReadOnly 及以上，operate 面
// （execute/采样触发/kick-session/set-draining）需 Operator 及以上，
// administer 面（terminate/config apply/shutdown）需 Admin。未绑定角色
// 的调用方无任何动作可用；角色拒绝与策略拒绝同权留痕（审计 + 计数 +
// kError 原因串）。
class MachineDaemon final : private IProfileMetaSource {
public:
    // execute 受控命令策略（权限边界，04-ops-control-plane MVP 的
    // “少量受控命令”）：
    // - trustedComponents：允许发起 execute 的来源组件白名单；
    // - allowedCommands：允许执行的命令名白名单（start/stop/restart…）。
    // 两个集合都为空集语义 = 一律拒绝（安全缺省：未显式授权即不可执行）。
    // 拒绝照常记审计（accepted=false）。策略门与 §6.1 角色门叠加：
    // 过策略门的调用方还需具备该动作所需角色（execute 需 Operator）。
    struct ExecPolicy final {
        std::vector<runtime::ComponentId> trustedComponents;
        std::vector<std::string> allowedCommands;
    };

    // 主机级非受控进程治理策略（06 §7 MVP "process list / state / pid" 的
    // 控制面切片）。与 ExecPolicy 刻意分离——授权口径与命令白名单互不
    // 牵动：execute 是“本代理受管进程的编排”，治理是对主机上其他进程
    // 的越权面，两者风险等级与审计语义不同，不得共享白名单。
    // - trustedComponents：允许发起枚举/处置的来源白名单；
    // - killableNames：处置目标进程名（/proc/<pid>/comm，精确匹配）
    //   白名单；空集 = 处置一律拒绝（安全缺省：未显式授权即不可处置）。
    // 枚举与处置共用来源白名单；处置额外受名单约束。所有尝试（含拒绝）
    // 照记审计（accepted=false）并推中心。
    struct ProcessGovernPolicy final {
        std::vector<runtime::ComponentId> trustedComponents;
        std::vector<std::string> killableNames;
    };

    // 诊断采样入口策略 DiagnosticsPolicy（04 §7）上提至命名空间级
    // （ProfileRelay.h）：中心侧查询/下载沿用同一读位（canAccess），
    // 授权口径不复制不走样；canTrigger 写位只在 agent 侧生效。

    // §6.1 节点生命周期/状态命令策略（machine.kick-session /
    // machine.set-draining / machine.shutdown）。与 ExecPolicy（受管
    // 进程编排）、ProcessGovernPolicy（主机非受控进程）刻意分离——
    // 授权口径与风险等级互不牵动，不得共享白名单。本族命令沿用 execute
    // 的双门模式：trustedComponents 来源白名单（空集 = 一律拒绝，安全
    // 缺省）+ AccessRole 角色绑定表（drain/kick 需 Operator，shutdown
    // 需 Admin，见 AccessControl.h）。所有尝试（含拒绝）照记审计。
    struct NodeOpsPolicy final {
        std::vector<runtime::ComponentId> trustedComponents;
    };

    struct Config final {
        std::string listenHost = "127.0.0.1";
        std::uint16_t listenPort = 0;  // 0 = 内核分配随机端口
        runtime::ComponentId componentId = 60;  // Machine 组件默认 id
        std::size_t auditCapacity = 128;  // 本地审计环形容量；0 = 关闭本地环形
        ExecPolicy execPolicy;
        // 主机级非受控进程治理策略（machine.processes / machine.terminate）；
        // 缺省全拒（来源与目标名单均为空集）。
        ProcessGovernPolicy processGovernPolicy;
        // §6.1 节点生命周期/状态命令策略（kick-session / set-draining /
        // shutdown）；缺省全拒（来源名单空集）。
        NodeOpsPolicy nodeOpsPolicy;
        // 会话登记真实面（foundation/SessionStore，redis 共享、跨进程
        // 可见——LoginApp 写入，本 daemon 吊销即全集群生效）：kick 入口
        // 的存储出口。nullptr = kick 入口关闭（machine.kick-session 视
        // 同未知方法，与采样入口同口径）。不持有；生命周期由调用方保证。
        foundation::SessionStore* sessionStore = nullptr;
        // 诊断采样入口策略（machine.profile.*）；缺省全拒。
        DiagnosticsPolicy diagnosticsPolicy;
        // §6.1 权限分级绑定表（来源组件 → 角色；口径见 AccessControl.h）。
        // 与既有策略门叠加：动作先过各自策略白名单（execute/govern/
        // diagnostics），再过角色门——inspect 面需 ReadOnly 及以上，
        // operate 面需 Operator 及以上，administer 面需 Admin。缺省
        // 空表 = 所有动作（含只读）一律拒绝（安全缺省）。
        std::vector<RoleBinding> roleBindings;
        // 采样能力出口（ITickProfiler，runtime 侧 TickProfiler 实现，
        // 由宿主把它挂到本进程 TickScheduler）：nullptr = 采样入口关闭
        // （machine.profile.* 视同未知方法）。不持有；生命周期由调用方
        // 保证。
        runtime::ITickProfiler* tickProfiler = nullptr;
        // 节点摘要上报周期；0 = 关闭周期上报。到期即采一次快照推给
        // reportSink（首个 tick 立即上报，保证中心侧新鲜度）。
        std::chrono::milliseconds reportInterval{0};
        INodeReportSink* reportSink = nullptr;  // 不持有；生命周期由调用方保证
        // 审计聚合出口（04 §8 MVP"操作审计"）：所有 execute 尝试与拒绝
        // 逐条推给中心（含 nodeId 归属）；本地环形容量与之正交。
        INodeAuditSink* auditSink = nullptr;  // 不持有；生命周期由调用方保证
        // 剖面产物回传出口（04 §7 中心持有副本）：tick 里发现新固化产物
        // 即推送（含元数据与只读字节）。nullptr = 不回传，中心侧下载
        // 无副本可用（诚实报无副本）。不持有；生命周期由调用方保证。
        INodeArtifactSink* artifactSink = nullptr;
    };

    MachineDaemon(Config config, IMachineAgent& agent);
    ~MachineDaemon();

    MachineDaemon(const MachineDaemon&) = delete;
    MachineDaemon& operator=(const MachineDaemon&) = delete;

    bool start();
    void stop();
    void tick();

    bool isListening() const;
    std::uint16_t localPort() const;

    // 本地审计视图（时间升序）；machine.audit 的 JSON 与此同源。
    const std::vector<AuditEntry>& auditLog() const;

private:
    // 上线登记：有中心出口（上报/审计聚合）才采样本机身份；有上报出口
    // 且身份非空则向中心注册占位（详情见 start()）。
    void announceToCenter();
    void acceptConnections();
    void processMessages();
    void handleInvocation(runtime::RuntimeInvocation& inv);
    void handleAudit(runtime::RuntimeInvocation& inv);
    void handleProcesses(runtime::RuntimeInvocation& inv);
    void handleTerminate(runtime::RuntimeInvocation& inv);
    void handleProfileTrigger(runtime::RuntimeInvocation& inv);
    void handleProfiles(runtime::RuntimeInvocation& inv);
    void handleProfileDownload(runtime::RuntimeInvocation& inv);
    void handleConfigApply(runtime::RuntimeInvocation& inv);
    void handleKickSession(runtime::RuntimeInvocation& inv);
    void handleSetDraining(runtime::RuntimeInvocation& inv);
    void handleShutdown(runtime::RuntimeInvocation& inv);
    bool isTrustedSource(runtime::ComponentId source) const;
    bool isCommandAllowed(const std::string& command) const;
    bool isGovernTrustedSource(runtime::ComponentId source) const;
    bool isKillableName(const std::string& name) const;
    bool isNodeOpsTrusted(runtime::ComponentId source) const;
    bool isTriggerAuthorized(runtime::ComponentId source) const;
    bool isDiagnosticsAccessAuthorized(runtime::ComponentId source) const;
    // §6.1 角色门：来源绑定角色达到 required 档位（未绑定 = None = 全拒）。
    bool hasRole(runtime::ComponentId source, AccessRole required) const;
    void appendAudit(const AuditEntry& entry);
    void reportIfDue();
    // 剖面回传：发现新固化产物即推给 artifactSink（见 tick()）。
    void relayArtifacts();
    // IProfileMetaSource：本机当前剖面清单（report() 通道快照语义）。
    std::vector<ProfileMeta> profileMetas() const override;
    void sendResponse(runtime::ComponentId target,
                      const std::string& method,
                      std::span<const std::byte> payload);

    Config config_;
    IMachineAgent& agent_;
    runtime::TcpListener listener_;
    std::shared_ptr<runtime::TransportHub> hub_;
    std::vector<AuditEntry> auditLog_;
    std::chrono::steady_clock::time_point lastReportAt_{};
    // 本机身份（快照 hostname，06 的 nodeId 口径）；空 = 未向中心登记。
    // start() 采样、stop() 注销沿用同一值——注销必与注册同键。
    std::string nodeId_;
    // 已回传中心的产品句柄账本（只留仍在产物环形里的——句柄不复用，
    // 被逐出者不会复现；上界 = 产物环形容量）。
    std::vector<std::uint64_t> relayedHandles_;
    // §6.1 受控停机：RPC 应答出站后置位，本 tick 收尾（processMessages
    // 之后、周期上报之前）执行优雅停机——注销中心 + 关听。延迟到 tick
    // 末是为不悬空消息处理上下文（hub 不得在分发中途销毁）。
    bool shutdownRequested_ = false;
};

}  // namespace theseed::control::machine
