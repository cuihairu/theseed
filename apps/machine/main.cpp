#include "theseed/control/machine/MachineAgent.h"
#include "theseed/control/machine/MachineDaemon.h"
#include "theseed/control/machine/MachineSnapshotCodec.h"

#include "AppMainUtil.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#define THESEED_ARGC __argc
#define THESEED_ARGV __argv
#else
#define THESEED_ARGC argc
#define THESEED_ARGV argv
#endif

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

using theseed::control::machine::MachineAgent;
using theseed::control::machine::LocalHostProbe;
using theseed::control::machine::LocalProcessSupervisor;
using theseed::control::machine::formatSnapshotJson;
using theseed::control::machine::formatSnapshotText;

// 控制面 RPC/遥测生命周期载体：MachineDaemon 才是实际承载（listen/分发/
// 审计/上报/OTLP），本入口负责 start/stop/tick 驱动。带 command 的一次性
// 调用保持旧 CLI 契约（执行后打印快照退出）；无 command 则作为 daemon
// 常驻，由 SignalHandler 驱动优雅停机。
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    theseed::app::installSignalHandlers();

    bool useJson = false;
    std::string command;
    std::string argument;
    std::string listenHost = "127.0.0.1";
    std::uint16_t listenPort = 0;  // 0 = 内核分配随机端口

    for (int index = 1; index < THESEED_ARGC; ++index) {
        const std::string current = THESEED_ARGV[index];
        if (current == "--json") {
            useJson = true;
            continue;
        }
        if (current == "--host" || current == "--port") {
            // 缺值显式报错退出：选项名不得落回 command 分支被当命令执行
            //（否则 daemon 意图被静默误判为一次性模式）。
            if (index + 1 >= THESEED_ARGC) {
                std::cerr << "missing value for " << current << "\n";
                return 1;
            }
            if (current == "--host") {
                listenHost = THESEED_ARGV[++index];
                continue;
            }
            const auto parsed = theseed::app::parsePort(THESEED_ARGV[++index]);
            if (!parsed) return 1;
            listenPort = *parsed;
            continue;
        }

        if (command.empty()) {
            command = current;
        } else if (argument.empty()) {
            argument = current;
        }
    }

    MachineAgent agent(
        std::make_unique<LocalHostProbe>(),
        std::make_unique<LocalProcessSupervisor>());

    // 一次性 CLI 模式（旧契约）：执行命令后打印快照并退出，不驻留。
    if (!command.empty()) {
        const bool ok = agent.execute(command, argument);
        std::cout << "command=" << command << "\n";
        std::cout << "command_ok=" << (ok ? "true" : "false") << "\n";

        const auto summary = agent.snapshot();
        std::cout << "theseed_machine bootstrap\n";
        if (useJson) {
            std::cout << formatSnapshotJson(summary) << "\n";
        } else {
            std::cout << formatSnapshotText(summary);
        }
        return 0;
    }

    // daemon 模式：打印 bootstrap 快照横幅后启动 MachineDaemon 并驱动
    // tick 循环（策略/角色/OTLP 全缺省安全值：空策略 = 一律拒绝）。
    // stdout 只承载快照——--json 时即纯 JSON 流（与旧版裸 --json 行为
    // 对齐，可脚本解析）；标题与生命周期行走 stderr，不混入 stdout。
    // 退出条件 = 信号停机（gRunning 清零）或 RPC 受控停机（machine.shutdown
    // 已受理）。
    const auto banner = agent.snapshot();
    if (useJson) {
        std::cerr << "theseed_machine bootstrap\n";
        std::cout << formatSnapshotJson(banner) << "\n";
    } else {
        std::cout << "theseed_machine bootstrap\n";
        std::cout << formatSnapshotText(banner);
    }

    theseed::control::machine::MachineDaemon::Config config;
    config.listenHost = listenHost;
    config.listenPort = listenPort;

    theseed::control::machine::MachineDaemon daemon(std::move(config), agent);
    if (!daemon.start()) {
        std::cerr << "machine.daemon start failed on " << listenHost << ":" << listenPort << "\n";
        return 1;
    }
    std::cerr << "machine.daemon listening on " << listenHost << ":" << daemon.localPort() << "\n";

    while (theseed::app::gRunning && !daemon.shutdownRequested()) {
        daemon.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    daemon.stop();
    std::cerr << "machine.daemon stopped.\n";
    return 0;
}
