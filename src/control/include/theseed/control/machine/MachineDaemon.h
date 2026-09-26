#pragma once

#include "theseed/control/machine/MachineAgent.h"
#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/TcpListener.h"
#include "theseed/runtime/TransportHub.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace theseed::control::machine {

// 控制面 RPC 方法名（RuntimeInvocation.method）。
namespace MachineMethod {
inline constexpr const char* kSnapshot = "machine.snapshot";
inline constexpr const char* kSnapshotOk = "machine.snapshot.ok";
inline constexpr const char* kExecute = "machine.execute";
inline constexpr const char* kExecuteOk = "machine.execute.ok";
// 统一错误响应：payload 为短原因串（可读，供人工诊断与测试断言）。
inline constexpr const char* kError = "machine.error";
}  // namespace MachineMethod

// MachineAgent 的 RPC 输出：把 snapshot/execute 暴露为控制面 TCP 端点。
//
// 协议复用组件间 RuntimeInvocation 帧（与 DBApp 同一族，不另起协议）：
//   machine.snapshot → machine.snapshot.ok  payload = 快照 JSON
//                      （MachineSnapshotCodec::formatSnapshotJson）
//   machine.execute  → machine.execute.ok   payload = 1 字节 0x01/0x00
//                      请求 payload = command '\0' args（命令与参数以
//                      NUL 分隔，与 IMachineAgent::execute 的两参对齐）
//   其余 method 或畸形 execute 载荷 → machine.error（payload = 原因串）
//
// 服务端连接由对端首条请求的 sourceComponent 自报注册
// （attachServerTransport，与 DBApp 同机制）。调用方负责在自身 tick
// 循环里泵 tick()：accept 新连接 + 收发分发一轮，全部非阻塞。
class MachineDaemon final {
public:
    struct Config final {
        std::string listenHost = "127.0.0.1";
        std::uint16_t listenPort = 0;  // 0 = 内核分配随机端口
        runtime::ComponentId componentId = 60;  // Machine 组件默认 id
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

private:
    void acceptConnections();
    void processMessages();
    void handleInvocation(runtime::RuntimeInvocation& inv);
    void sendResponse(runtime::ComponentId target,
                      const std::string& method,
                      std::span<const std::byte> payload);

    Config config_;
    IMachineAgent& agent_;
    runtime::TcpListener listener_;
    std::shared_ptr<runtime::TransportHub> hub_;
};

}  // namespace theseed::control::machine
