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
#include <sys/resource.h>
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

// 断言失败沿 FAIL 宏直接 return，子进程若不随行清理会在残余存活期
// （120s×1000 tick/s 的密跑循环）持续消耗 CPU——在高载环境下一次失败
// 就放大成后续运行的负载源。守卫保证任何退出路径都 stop。
struct ChildStopGuard {
    LocalProcessSupervisor* supervisor;
    std::uint32_t pid = 0;
    ~ChildStopGuard() {
        if (pid != 0) supervisor->stop(pid);
    }
};

// 极简 accept-and-respond 服务器：单连接，发送 reply 后关闭。
// 线程由调用方 join——不能 detach：accept 线程迟迟未跑时 LISTEN fd 尚
// 未关闭，此后的 supervisor.start() fork 会把该 fd 继承给子进程，而
// scanListeningPorts 按「最小监听端口」反查会命中这个无主 listener
// （无人 accept，版本探测恒超时）——2026-09-28 flake 根因。调用方在
// 该用例结束（尤其 fork 前）必须 join。
struct ReplyServer {
    std::uint16_t port = 0;
    std::thread thread;
};

ReplyServer startReplyServer(const std::string& reply) {
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return {};
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(s, 4) != 0) {
        ::close(s);
        return {};
    }
    socklen_t len = sizeof(addr);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);

    ReplyServer server;
    server.port = ntohs(addr.sin_port);
    server.thread = std::thread([s, reply] {
        const int c = ::accept(s, nullptr, nullptr);
        if (c >= 0) {
            ::send(c, reply.data(), reply.size(), 0);
            ::close(c);
        }
        ::close(s);
    });
    return server;
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

// 子进程模式：挂一个真实 OpsServer（版本 9.9.9-child）运行 ~120s 后退出，
// 供 supervisor 端到端测试反查 port 与探测 version。存活上界须覆盖主进程
// 复验制轮询的预算上限（2×30s），否则预算用满时的 stop 会撞上已自退的
// 子进程。tick 以 1ms 粒度密跑：探测 miss 会在 backlog（TcpListener 默认
// 16）滞留半开连接，慢 tick 下滞留堆积可 saturate 队列，此后新 connect
// 撞 SYN 重传（探测的 connect 无超时）拖出数十秒慢轮——高 tick 率让
// 滞留连接被快速 accept+清理，排空速度远高于主进程的探测产生速率。
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
    for (int i = 0; i < 120000; ++i) {
        server.tick();
        ::usleep(1000);
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

    TEST("probeProcessVersion connect times out against saturated backlog");
    {
        // accept 队列塞满（backlog=1 + 预占连接不 accept）后，内核对后续
        // SYN 静默丢弃，探测的 connect 停在 SYN_SENT——建连超时臂的真实
        // 路径（阻塞式 connect 在此会撞 SYN 重传卡秒级，非阻塞 + poll 满
        // 短超时返回空）。耗时断言钉住「超时确实生效」而非即时拒绝。
        TcpListener listener;
        if (!listener.listen("127.0.0.1", 0, 1))
            FAIL("backlog listener failed to bind");

        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(listener.localPort());
        target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int fillers[2] = {-1, -1};
        for (int& filler : fillers) {
            filler = ::socket(AF_INET, SOCK_STREAM, 0);
            if (filler < 0) FAIL("filler socket failed");
            if (::connect(filler, reinterpret_cast<sockaddr*>(&target),
                          sizeof(target)) != 0) {
                ::close(filler);
                filler = -1;
                FAIL("filler connect failed");
            }
        }

        const auto begin = std::chrono::steady_clock::now();
        const auto version =
            probeProcessVersion(listener.localPort(), std::chrono::milliseconds{60});
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - begin)
                .count();
        for (int filler : fillers) {
            if (filler >= 0) ::close(filler);
        }
        listener.close();
        if (!version.empty()) FAIL("saturated backlog should give empty");
        if (elapsed < 50)
            FAIL("connect timeout not honored (elapsed " +
                 std::to_string(elapsed) + "ms)");
        PASS();
    }

    TEST("probeProcessVersion without version field or truncated value returns empty");
    {
        auto plain = startReplyServer("HTTP/1.0 200 OK\r\n\r\nplain text, no json");
        if (!probeProcessVersion(plain.port).empty())
            FAIL("response without version must give empty");
        plain.thread.join();  // fork 前确保 LISTEN fd 已随线程关闭

        auto truncated = startReplyServer("HTTP/1.0 200 OK\r\n\r\n{\"version\":\"1.2");
        if (!probeProcessVersion(truncated.port).empty())
            FAIL("truncated value must give empty");
        truncated.thread.join();
        PASS();
    }

    TEST("probeProcessVersion yields empty when the fd limit is exhausted");
    {
        // fd<0 臂的补测收口：RLIMIT_NOFILE 软限压 0 使 socket() 恒
        // EMFILE，资源耗尽臂稳定注入（原登记「无法稳定注入」翻案）。
        // 端口必须先于限额变更取得（freePort 自身要开 socket）；断言
        // 前恢复限额，失败路径不污染后续用例。
        const auto port = freePort();
        rlimit oldLimit{};
        if (::getrlimit(RLIMIT_NOFILE, &oldLimit) != 0)
            FAIL("getrlimit failed");
        rlimit zeroLimit = oldLimit;
        zeroLimit.rlim_cur = 0;
        if (::setrlimit(RLIMIT_NOFILE, &zeroLimit) != 0)
            FAIL("setrlimit(0) failed");
        const auto version = probeProcessVersion(port);
        ::setrlimit(RLIMIT_NOFILE, &oldLimit);
        if (!version.empty())
            FAIL("exhausted fd limit must give empty");
        PASS();
    }

    TEST("probeProcessVersion aborts reading an oversized response");
    {
        // >64KB 越界臂的补测收口：回环应答端回 70KB 无版本正文，读
        // 循环的总量守卫触发 break（原登记「/health 恒小于 2KB」翻案
        // ——守卫的语义本就是为这类异常响应兜底）。正文无版本键 →
        // 探测返回空。
        std::string giant = "HTTP/1.0 200 OK\r\nContent-Length: 70000\r\n\r\n";
        giant.append(70000, 'x');
        auto oversized = startReplyServer(giant);
        if (!probeProcessVersion(oversized.port).empty())
            FAIL("oversized response without version must give empty");
        oversized.thread.join();
        PASS();
    }

    // --- LocalProcessSupervisor 端到端：受管子进程补 port 与 version ---
    TEST("listProcesses reports managed child port and version end to end");
    {
        LocalProcessSupervisor supervisor;
        ChildStopGuard childGuard{&supervisor};
        if (!supervisor.start("/proc/self/exe --child-ops"))
            FAIL("failed to start child ops mode");

        // 子进程启动 + OpsServer 监听 + 探测需要毫秒级；轮询快照直到
        // 受管子进程补出端口与版本，命中即拷出快照。轮询成本是环境敏
        // 感的：listProcesses 单轮要对全机 /proc 逐 pid 扫 fd（本机实
        // 测 545 pid），与其他 ctest 并行时单轮可阻塞数秒、子进程 tick
        // 饥饿使单次 500ms 版本探测偶发 miss——浅预算会把偶发负载放
        // 大成必然失败（2026-09-28 flake 根因）。环境毛刺纪律（与
        // HostProbeTest::testRealProcSources 同款）：断言一字不动，首
        // 预算未命中时取完整预算复验一次，连续两预算未命中才判失败。
        // 轮首查预算（不发预算外的新轮）、轮尾也查（慢轮后不再续睡）。
        const auto pollUntilComplete = [&](ProcessSummary& found,
                                           int& polls) -> bool {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds{30};
            auto interval = std::chrono::milliseconds{250};
            while (std::chrono::steady_clock::now() < deadline) {
                for (const auto& process : supervisor.listProcesses()) {
                    if (process.managed) {
                        found = process;
                        childGuard.pid = process.pid;
                    }
                }
                ++polls;
                if (found.port != 0 && !found.version.empty()) return true;
                if (std::chrono::steady_clock::now() >= deadline) break;
                // 间隔退避（250ms→500ms→1s 封顶）：每次 miss 的探测都会
                // 在子进程 pending_/backlog 双层队列滞留半开连接（见
                // runChildOpsMode 注释），间隔过密等于自造 SYN 积压；退
                // 避给子进程的 accept 排空留出窗口。
                std::this_thread::sleep_for(interval);
                if (interval < std::chrono::seconds{1}) interval *= 2;
            }
            return false;
        };

        ProcessSummary found;
        int polls = 0;
        if (pollUntilComplete(found, polls)) {
            // 首预算命中：定论臂在下方统一断言。
        } else {
            std::cout << "  (slow environment, re-polling with a fresh budget) "
                      << std::flush;
            pollUntilComplete(found, polls);
        }

        if (found.port == 0 || found.version.empty())
            FAIL("managed child listening port not discovered (polls=" +
                 std::to_string(polls) + " port=" + std::to_string(found.port) +
                 " version='" + found.version + "')");
        if (found.version != "9.9.9-child")
            FAIL("child version mismatch: '" + found.version + "'");

        if (!supervisor.stop(found.pid)) FAIL("failed to stop child");
        PASS();
    }

    std::cout << "\nAll ProcessPortScanner tests passed!" << std::endl;
    return 0;
}
