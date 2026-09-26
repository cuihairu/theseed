// ProcessPortScanner 测试：
// - collectListenInodes 用合成流驱动畸形行分支（表头/非 LISTEN/列数不足/
//   无冒号地址/零端口/tcp6 行/inode 覆盖）；
// - scanListeningPorts / probeProcessVersion 用真实 /proc 与 TCP 回环驱动：
//   本进程监听端口反查、拒连端口、静默监听超时、无版本/截断响应、活体
//   OpsServer 版本往返；
// - LocalProcessSupervisor 端到端：自 exec 的子进程模式挂真实 OpsServer，
//   listProcesses 补出受管子进程的 port 与 version（todo 遗留事项
//   “端口占用扫描与二进制版本探测”的整链验收）。
#include "theseed/control/machine/ProcessPortScanner.h"
#include "theseed/control/machine/ProcessSupervisor.h"
#include "theseed/ops/OpsInspector.h"
#include "theseed/ops/OpsServer.h"
#include "theseed/runtime/TcpListener.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

using theseed::control::machine::LocalProcessSupervisor;
using theseed::control::machine::ProcessSummary;
using theseed::control::machine::collectListenInodes;
using theseed::control::machine::probeProcessVersion;
using theseed::control::machine::scanListeningPorts;
using theseed::ops::OpsInspector;
using theseed::ops::OpsServer;
using theseed::ops::ProcessInfo;
using theseed::runtime::TcpListener;

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

std::uint16_t freePort() {
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(s);
        return 0;
    }
    socklen_t len = sizeof(addr);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
    ::close(s);
    return ntohs(addr.sin_port);
}

// 极简 accept-and-respond 服务器：单连接，发送 reply 后关闭。
// 线程分离自回收；端口由调用方先 bind 探测，竞态窗口可忽略（测试串行）。
std::uint16_t startReplyServer(const std::string& reply) {
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(s, 4) != 0) {
        ::close(s);
        return 0;
    }
    socklen_t len = sizeof(addr);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
    const auto port = ntohs(addr.sin_port);

    std::thread([s, reply] {
        const int c = ::accept(s, nullptr, nullptr);
        if (c >= 0) {
            ::send(c, reply.data(), reply.size(), 0);
            ::close(c);
        }
        ::close(s);
    }).detach();
    return port;
}

// 阻塞式探测跑在子线程，主线程持续 tick OpsServer 驱动 accept/响应。
std::string probeWhileTicking(OpsServer& server, std::uint16_t port,
                              std::chrono::milliseconds timeout) {
    std::string version;
    std::atomic<bool> done{false};
    std::thread prober([&] {
        version = probeProcessVersion(port, timeout);
        done.store(true);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        server.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    prober.join();
    return version;
}

// 子进程模式：挂一个真实 OpsServer（版本 9.9.9-child）运行 ~30s 后退出，
// 供 supervisor 端到端测试反查 port 与探测 version。
int runChildOpsMode() {
    ProcessInfo info;
    info.role = "ChildProbe";
    info.version = "9.9.9-child";
    OpsInspector inspector(std::move(info));
    OpsServer::Config config;
    config.host = "127.0.0.1";
    config.port = 0;
    OpsServer server(config, inspector);
    if (!server.start()) {
        return 1;
    }
    for (int i = 0; i < 3000; ++i) {
        server.tick();
        ::usleep(10000);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--child-ops") {
        return runChildOpsMode();
    }

    std::cout << "ProcessPortScannerTest:" << std::endl;

    // --- collectListenInodes：合成流驱动解析分支 ---
    TEST("collectListenInodes parses LISTEN rows and skips the rest");
    {
        std::istringstream stream(
            "  sl  local_address rem_address   st tx_queue rx_queue  tr:tm->when "
            "retrnsmt   uid  timeout inode\n"
            "   0: 0100007F:1F90 00000000:0000 0A 00000000:00000000 00:00000000 "
            "00000000  1000        0 111111 1 ffff\n"   // LISTEN 8080
            "   1: 0100007F:1F91 00000000:0000 01 00000000:00000000 00:00000000 "
            "00000000  1000        0 222222 1 ffff\n"   // ESTABLISHED：跳过
            "   2: noColonAddr 00000000:0000 0A x y z w 8 9 333333\n"  // 地址无冒号
            "   3: 0100007F:0000 00000000:0000 0A 00000000:00000000 00:00000000 "
            "00000000  1000        0 444444 1 ffff\n"   // 零端口
            "   short row\n"                             // 列数不足
            "   4: 00000000000000000000000000000000:0050 0000000000000000000"
            "00000000000000:0000 0A 00000000:00000000 00:00000000 00000000     0 "
            "        0 555555 1 ffff\n"                  // tcp6 LISTEN 80
            "   5: 0100007F:0050 00000000:0000 0A 00000000:00000000 00:00000000 "
            "00000000  1000        0 555556 1 ffff\n"   // 与 tcp6 同端口不同 inode
            "   6: 0100007F:1F90 00000000:0000 0A 00000000:00000000 00:00000000 "
            "00000000  1000        0 111111 1 ffff\n");  // 同 inode 覆盖为 8080

        std::unordered_map<std::string, std::uint16_t> inodes;
        collectListenInodes(stream, inodes);
        // 四条 LISTEN 行里 444444 是零端口（跳过），555555/555556 是同端口
        // 的 tcp6 与 tcp 变体（各自入表），共 3 项。
        if (inodes.size() != 3) FAIL("expected 3 listen inodes, got " +
                                     std::to_string(inodes.size()));
        if (inodes.at("111111") != 0x1F90) FAIL("inode 111111 port mismatch");
        if (inodes.at("555555") != 80) FAIL("tcp6 inode port mismatch");
        if (inodes.at("555556") != 80) FAIL("decimal-looking hex port mismatch");
        if (inodes.count("222222") != 0 || inodes.count("333333") != 0 ||
            inodes.count("444444") != 0)
            FAIL("non-LISTEN/malformed rows leaked into map");
        PASS();
    }

    TEST("collectListenInodes on empty stream yields nothing");
    {
        std::istringstream empty("");
        std::unordered_map<std::string, std::uint16_t> inodes;
        collectListenInodes(empty, inodes);
        if (!inodes.empty()) FAIL("empty stream should yield empty map");
        PASS();
    }

    // --- scanListeningPorts：真实 /proc 反查 ---
    TEST("scanListeningPorts maps own listening socket and skips unknown pids");
    {
        TcpListener listener;
        if (!listener.listen("127.0.0.1", 0)) FAIL("test listener failed to bind");

        const std::vector<std::uint32_t> pids = {
            static_cast<std::uint32_t>(::getpid()), 4'000'000u};
        const auto ports = scanListeningPorts(pids);
        const auto own = ports.find(static_cast<std::uint32_t>(::getpid()));
        if (own == ports.end() || own->second != listener.localPort())
            FAIL("own listener port not discovered");
        if (ports.count(4'000'000u) != 0) FAIL("bogus pid must not map");
        listener.close();
        PASS();
    }

    TEST("scanListeningPorts with empty pid list returns empty");
    {
        if (!scanListeningPorts({}).empty()) FAIL("empty pids should give empty map");
        PASS();
    }

    TEST("scanListeningPorts with dead pid returns no entry");
    {
        const auto ports = scanListeningPorts({4'000'000u});
        if (!ports.empty()) FAIL("dead pid should not map");
        PASS();
    }

    // --- probeProcessVersion：真实 TCP 回环 ---
    TEST("probeProcessVersion extracts version from live OpsServer");
    {
        ProcessInfo info;
        info.role = "ProbeTarget";
        info.version = "7.7.7-probe";
        OpsInspector inspector(std::move(info));
        OpsServer::Config config;
        config.host = "127.0.0.1";
        config.port = 0;
        OpsServer server(config, inspector);
        if (!server.start()) FAIL("ops server failed to start");

        const auto version = probeWhileTicking(server, server.localPort(),
                                               std::chrono::milliseconds{500});
        if (version != "7.7.7-probe")
            FAIL("expected 7.7.7-probe, got '" + version + "'");
        server.stop();
        PASS();
    }

    TEST("probeProcessVersion on port 0 and refused port returns empty");
    {
        if (!probeProcessVersion(0).empty()) FAIL("port 0 must give empty");
        if (!probeProcessVersion(freePort()).empty())
            FAIL("refused port must give empty");
        PASS();
    }

    TEST("probeProcessVersion times out against a silent listener");
    {
        TcpListener listener;
        if (!listener.listen("127.0.0.1", 0)) FAIL("silent listener failed to bind");
        // 无人 accept：连接停在 backlog，recv 等满短超时后失败返回空。
        const auto version =
            probeProcessVersion(listener.localPort(), std::chrono::milliseconds{150});
        if (!version.empty()) FAIL("silent listener should give empty");
        listener.close();
        PASS();
    }

    TEST("probeProcessVersion without version field or truncated value returns empty");
    {
        const auto plainPort =
            startReplyServer("HTTP/1.0 200 OK\r\n\r\nplain text, no json");
        if (!probeProcessVersion(plainPort).empty())
            FAIL("response without version must give empty");

        const auto truncatedPort =
            startReplyServer("HTTP/1.0 200 OK\r\n\r\n{\"version\":\"1.2");
        if (!probeProcessVersion(truncatedPort).empty())
            FAIL("truncated value must give empty");
        PASS();
    }

    // --- LocalProcessSupervisor 端到端：受管子进程补 port 与 version ---
    TEST("listProcesses reports managed child port and version end to end");
    {
        LocalProcessSupervisor supervisor;
        if (!supervisor.start("/proc/self/exe --child-ops"))
            FAIL("failed to start child ops mode");

        // 子进程启动 + OpsServer 监听 + 探测需要毫秒级；轮询快照直到
        // 受管子进程补出端口与版本（带 10s 兜底），命中即拷出快照。
        ProcessSummary found;
        bool complete = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (std::chrono::steady_clock::now() < deadline) {
            for (const auto& process : supervisor.listProcesses()) {
                if (process.managed) found = process;
            }
            complete = found.port != 0 && !found.version.empty();
            if (complete) break;
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }

        if (!complete || found.port == 0)
            FAIL("managed child listening port not discovered");
        if (found.version != "9.9.9-child")
            FAIL("child version mismatch: '" + found.version + "'");

        if (!supervisor.stop(found.pid)) FAIL("failed to stop child");
        PASS();
    }

    std::cout << "\nAll ProcessPortScanner tests passed!" << std::endl;
    return 0;
}
