// theseed-bots：批量协议机器人压测 CLI（KBEngine bots 的斜坡加压思想）。
//
// 用法：theseed-bots --config bots.ini
// 配置为 key = value 文本（见 BotsConfig.h 字段注释）。
#include "BotsConfig.h"
#include "BotManager.h"

#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

namespace {

volatile bool g_running = true;

void signalHandler(int) {
    g_running = false;
}

void printUsage() {
    std::cout
        << "theseed-bots — batch protocol bots / ramp load driver\n"
        << "\n"
        << "Usage: theseed-bots --config <file>\n"
        << "\n"
        << "Config: key = value lines ('#' / ';' comments). Keys:\n"
        << "  login_host login_port base_host base_port realm_id\n"
        << "  account_prefix account_password\n"
        << "  total_count tick_time tick_count          # ramp: every tick_time ms add tick_count bots\n"
        << "  move_interval heartbeat_interval           # per-bot action script (ms)\n"
        << "  move_action heartbeat_action               # EntityDef method names\n"
        << "  reconnect_delay stats_interval run_time tick_interval\n"
        << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::string configPath;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            configPath = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg
                      << " (see theseed-bots --help)" << std::endl;
            return 1;
        }
    }
    if (configPath.empty()) {
        printUsage();
        return 1;
    }

    theseed::bots::BotsConfig config;
    std::string error;
    if (!theseed::bots::loadBotsConfig(configPath, config, error)) {
        std::cerr << "config error: " << error << std::endl;
        return 1;
    }

    std::cout << "theseed-bots: login " << config.loginHost << ":" << config.loginPort
              << " realm=" << config.realmId
              << " ramp=" << config.totalCount << "/" << config.tickTime.count()
              << "ms/" << config.tickCount
              << " run_time=" << config.runTime.count() << "s" << std::endl;

    theseed::bots::BotManager manager(config);
    const auto start = std::chrono::steady_clock::now();
    auto lastPrint = start - config.statsInterval;  // 首个 tick 后立即打印一轮

    while (g_running) {
        const auto now = std::chrono::steady_clock::now();
        manager.tick(now);

        if (now - lastPrint >= config.statsInterval) {
            lastPrint = now;
            std::cout << "[bots] " << theseed::bots::renderStatsLine(manager.aggregate())
                      << std::endl;
        }
        if (config.runTime.count() > 0
            && now - start >= config.runTime) {
            break;
        }
        std::this_thread::sleep_for(config.tickInterval);
    }

    manager.stop();
    std::cout << "[bots] final " << theseed::bots::renderStatsLine(manager.aggregate())
              << std::endl;
    std::cout << "theseed-bots stopped." << std::endl;
    return 0;
}
