// MachineDaemon 端到端测试：真实 TCP 回环驱动 LocalHostProbe +
// LocalProcessSupervisor + MachineAgent 的完整链路——snapshot JSON 往返、
// execute start/stop 受管子进程闭环、协议错误（畸形 execute 载荷/未知方法）、
// 幂等 start 与端口冲突。
#include "theseed/control/machine/HostProbe.h"
#include "theseed/control/machine/MachineAgent.h"
#include "theseed/control/machine/MachineDaemon.h"
#include "theseed/control/machine/ProcessSupervisor.h"
#include "theseed/runtime/NetworkTransport.h"
#include "theseed/runtime/TcpConnection.h"

#include <arpa/inet.h>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using theseed::control::machine::LocalHostProbe;
using theseed::control::machine::LocalProcessSupervisor;
using theseed::control::machine::MachineAgent;
using theseed::control::machine::MachineDaemon;
// 命名空间不能 using-declare，用别名
namespace MachineMethod = theseed::control::machine::MachineMethod;
using theseed::runtime::ComponentId;
using theseed::runtime::NetworkTransport;
using theseed::runtime::RuntimeInvocation;
using theseed::runtime::SendResult;
using theseed::runtime::TcpConnection;

#define TEST(name)                            \
    do {                                      \
        std::cout << "  " << name << "... ";  \
    } while (0)
#define PASS() std::cout << "OK" << std::endl
#define FAIL(msg)                                   \
    do {                                            \
        std::cout << "FAILED: " << msg << std::endl; \
        return 1;                                   \
    } while (0)

namespace {

constexpr ComponentId kMachineComponent = 60;
constexpr ComponentId kClientComponent = 1;

// 裸协议客户端：直连 MachineDaemon 监听端口收发 RuntimeInvocation。
struct RawClient {
    std::shared_ptr<TcpConnection> conn;
    std::shared_ptr<NetworkTransport> transport;
    int pumpCount = 0;

    bool connect(std::uint16_t port) {
        conn = TcpConnection::create();
        if (!conn->connect("127.0.0.1", port)) return false;
        transport = std::make_shared<NetworkTransport>(conn);
        return true;
    }

    void settle(const std::function<void()>& daemonTick) {
        for (int i = 0; i < 40; ++i) {
            daemonTick();
            transport->tick();
            ::usleep(2000);
        }
        pumpCount = 0;
    }

    bool request(const std::string& method, std::vector<std::byte> payload,
                 const std::function<void()>& daemonTick, RuntimeInvocation& out) {
        RuntimeInvocation inv;
        inv.sourceComponent = kClientComponent;
        inv.targetComponent = kMachineComponent;
        inv.method = method;
        inv.payload = std::move(payload);
        if (transport->send(std::move(inv)) != SendResult::Accepted) return false;
        transport->flush();
        for (int i = 0; i < 4000; ++i) {
            if (++pumpCount > 20000) {
                std::cout << "FAILED: pump guard tripped" << std::endl;
                std::exit(1);
            }
            daemonTick();
            transport->tick();
            ::usleep(500);
            if (transport->receive(kClientComponent, &out, 1) > 0) return true;
        }
        return false;
    }
};

std::string payloadToString(const RuntimeInvocation& inv) {
    return std::string(reinterpret_cast<const char*>(inv.payload.data()),
                       inv.payload.size());
}

std::vector<std::byte> payloadOf(const std::string& text) {
    std::vector<std::byte> bytes(text.size());
    if (!text.empty()) {
        std::memcpy(bytes.data(), text.data(), text.size());
    }
    return bytes;
}

// execute 请求载荷：command '\0' args（不能用字符串字面量拼——string_view
// 构造在首个 NUL 截断）。
std::vector<std::byte> executePayload(const std::string& command, const std::string& args) {
    return payloadOf(command + std::string(1, '\0') + args);
}

// 从快照 JSON 提取受管进程的 pid：定位 "\"managed\":true"，向前回溯最近的
// "\"pid\":" 取数字（Codec 的进程对象字段顺序固定，测试只依赖本仓实现）。
std::uint32_t firstManagedPid(const std::string& json) {
    const auto managed = json.find("\"managed\":true");
    if (managed == std::string::npos) return 0;
    const auto pidKey = json.rfind("\"pid\":", managed);
    if (pidKey == std::string::npos) return 0;
    return static_cast<std::uint32_t>(
        std::strtoul(json.c_str() + pidKey + 6, nullptr, 10));
}

}  // namespace

int main() {
    std::cout << "MachineDaemonTest:" << std::endl;

    LocalHostProbe probe;
    auto supervisor = std::make_unique<LocalProcessSupervisor>();
    MachineAgent agent(std::make_unique<LocalHostProbe>(std::move(probe)),
                       std::move(supervisor));

    MachineDaemon::Config config;
    config.listenPort = 0;
    MachineDaemon daemon(config, agent);

    TEST("daemon starts, is idempotent, and reports its port");
    if (!daemon.start()) FAIL("daemon start failed");
    if (!daemon.start()) FAIL("second start must be idempotent-true");
    if (!daemon.isListening() || daemon.localPort() == 0)
        FAIL("daemon not listening after start");
    PASS();

    const auto port = daemon.localPort();
    auto daemonTick = [&daemon] { daemon.tick(); };

    RawClient client;
    TEST("client connects and settles");
    if (!client.connect(port)) FAIL("client connect failed");
    client.settle(daemonTick);
    PASS();

    TEST("machine.snapshot returns real host JSON");
    {
        RuntimeInvocation resp;
        if (!client.request(MachineMethod::kSnapshot, {}, daemonTick, resp))
            FAIL("no response to snapshot");
        if (resp.method != MachineMethod::kSnapshotOk)
            FAIL("wrong method: " + resp.method);
        const auto json = payloadToString(resp);
        if (json.find("\"hostname\":") == std::string::npos ||
            json.find("\"platform\":") == std::string::npos)
            FAIL("snapshot JSON missing host fields: " + json.substr(0, 120));
        PASS();
    }

    TEST("execute start spawns a managed child");
    {
        RuntimeInvocation resp;
        if (!client.request(MachineMethod::kExecute,
                            executePayload("start", "/bin/sleep 30"), daemonTick, resp))
            FAIL("no response to execute start");
        if (resp.method != MachineMethod::kExecuteOk)
            FAIL("wrong method: " + resp.method);
        if (resp.payload.size() != 1 || resp.payload[0] != std::byte{0x01})
            FAIL("execute start should report success");
        PASS();
    }

    TEST("snapshot shows the managed child with its pid");
    std::uint32_t childPid = 0;
    {
        RuntimeInvocation resp;
        if (!client.request(MachineMethod::kSnapshot, {}, daemonTick, resp))
            FAIL("no response to snapshot after start");
        const auto json = payloadToString(resp);
        if (json.find("\"managed\":true") == std::string::npos)
            FAIL("managed child missing from snapshot: " + json.substr(0, 120));
        childPid = firstManagedPid(json);
        if (childPid == 0) FAIL("failed to extract managed pid from snapshot");
        PASS();
    }

    TEST("execute stop terminates the child and clears managed state");
    {
        RuntimeInvocation resp;
        if (!client.request(MachineMethod::kExecute,
                            executePayload("stop", std::to_string(childPid)),
                            daemonTick, resp))
            FAIL("no response to execute stop");
        if (resp.method != MachineMethod::kExecuteOk ||
            resp.payload.size() != 1 || resp.payload[0] != std::byte{0x01})
            FAIL("execute stop should report success");

        if (!client.request(MachineMethod::kSnapshot, {}, daemonTick, resp))
            FAIL("no response to snapshot after stop");
        if (payloadToString(resp).find("\"managed\":true") != std::string::npos)
            FAIL("managed child should be gone after stop");
        PASS();
    }

    TEST("execute of unknown pid reports failure byte, not protocol error");
    {
        RuntimeInvocation resp;
        if (!client.request(MachineMethod::kExecute,
                            executePayload("restart", "4000000"), daemonTick, resp))
            FAIL("no response to execute restart");
        if (resp.method != MachineMethod::kExecuteOk)
            FAIL("expected execute.ok, got " + resp.method);
        if (resp.payload.size() != 1 || resp.payload[0] != std::byte{0x00})
            FAIL("unknown-pid restart should report failure byte");
        PASS();
    }

    TEST("malformed execute payload gets machine.error");
    {
        RuntimeInvocation resp;
        if (!client.request(MachineMethod::kExecute, payloadOf("start-no-separator"),
                            daemonTick, resp))
            FAIL("no response to malformed execute");
        if (resp.method != MachineMethod::kError) FAIL("expected machine.error");
        if (payloadToString(resp).find("NUL") == std::string::npos)
            FAIL("error payload should explain the reason");
        PASS();
    }

    TEST("empty command gets machine.error");
    {
        RuntimeInvocation resp;
        if (!client.request(MachineMethod::kExecute, executePayload("", "args"),
                            daemonTick, resp))
            FAIL("no response to empty command");
        if (resp.method != MachineMethod::kError) FAIL("expected machine.error");
        PASS();
    }

    TEST("unknown method gets machine.error");
    {
        RuntimeInvocation resp;
        if (!client.request("machine.nope", {}, daemonTick, resp))
            FAIL("no response to unknown method");
        if (resp.method != MachineMethod::kError) FAIL("expected machine.error");
        if (payloadToString(resp).find("unknown method") == std::string::npos)
            FAIL("error payload should name the method");
        PASS();
    }

    TEST("second daemon on the same port fails to start");
    {
        MachineDaemon::Config conflicting;
        conflicting.listenHost = "127.0.0.1";
        conflicting.listenPort = port;
        MachineDaemon second(conflicting, agent);
        if (second.start()) FAIL("conflicting start should fail");
        if (second.isListening()) FAIL("failed daemon must not be listening");
    }
    PASS();

    TEST("stop is safe and tick becomes a no-op");
    daemon.stop();
    if (daemon.isListening()) FAIL("daemon still listening after stop");
    daemon.tick();  // 不得崩溃
    PASS();

    std::cout << "\nAll MachineDaemon tests passed!" << std::endl;
    return 0;
}
