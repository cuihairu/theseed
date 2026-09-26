#pragma once

#include "theseed/control/machine/MachineAgent.h"
#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/TcpListener.h"
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
// 统一错误响应：payload 为短原因串（可读，供人工诊断与测试断言）。
inline constexpr const char* kError = "machine.error";
}  // namespace MachineMethod

// 受控命令审计条目（Ops Control Plane MVP 的“操作审计”）。execute 尝试与
// 协议层拒绝各记一条；snapshot 只读不记（避免噪声）。
struct AuditEntry {
    std::chrono::system_clock::time_point timestamp{};
    runtime::ComponentId source = 0;
    std::string command;   // 未知方法请求记 method 名
    std::string args;
    bool accepted = false;  // false = 协议层拒绝（畸形载荷/空命令/未知方法）
    bool ok = false;        // agent 执行结果（accepted 时有意义）
};

// MachineAgent 的 RPC 输出：把 snapshot/execute/audit 暴露为控制面 TCP 端点。
//
// 协议复用组件间 RuntimeInvocation 帧（与 DBApp 同一族，不另起协议）：
//   machine.snapshot → machine.snapshot.ok  payload = 快照 JSON
//                      （MachineSnapshotCodec::formatSnapshotJson）
//   machine.execute  → machine.execute.ok   payload = 1 字节 0x01/0x00
//                      请求 payload = command '\0' args（命令与参数以
//                      NUL 分隔，与 IMachineAgent::execute 的两参对齐）
//   machine.audit    → machine.audit.ok     payload = 审计条目 JSON 数组
//   其余 method 或畸形 execute 载荷 → machine.error（payload = 原因串）
//
// 每条 execute 尝试与协议层拒绝记入环形审计日志（容量 auditCapacity，
// 满后丢最旧；0 = 关闭审计）。
//
// 服务端连接由对端首条请求的 sourceComponent 自报注册
// （attachServerTransport，与 DBApp 同机制）。调用方负责在自身 tick
// 循环里泵 tick()：accept 新连接 + 收发分发一轮，全部非阻塞；审计日志
// 只在 tick 上下文写入，无锁（单线程假设与 DBApp 一致）。
class MachineDaemon final {
public:
    struct Config final {
        std::string listenHost = "127.0.0.1";
        std::uint16_t listenPort = 0;  // 0 = 内核分配随机端口
        runtime::ComponentId componentId = 60;  // Machine 组件默认 id
        std::size_t auditCapacity = 128;  // 审计环形容量；0 = 关闭审计
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
    void acceptConnections();
    void processMessages();
    void handleInvocation(runtime::RuntimeInvocation& inv);
    void handleAudit(runtime::RuntimeInvocation& inv);
    void appendAudit(const AuditEntry& entry);
    void sendResponse(runtime::ComponentId target,
                      const std::string& method,
                      std::span<const std::byte> payload);

    Config config_;
    IMachineAgent& agent_;
    runtime::TcpListener listener_;
    std::shared_ptr<runtime::TransportHub> hub_;
    std::vector<AuditEntry> auditLog_;
};

}  // namespace theseed::control::machine
