#include "theseed/db/DBApp.h"
#include "theseed/runtime/TickScheduler.h"

#include <chrono>
#include <csignal>
#include <iostream>
#include <sstream>
#include <thread>

static volatile bool g_running = true;

static void signalHandler(int) {
    g_running = false;
}

int main(int argc, char** argv) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    theseed::db::DBApp::Config config;
    config.listenPort = 20003;
    config.storePath = "data/entities";

    bool dbPortSet = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            config.listenPort = static_cast<std::uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--store" && i + 1 < argc) {
            config.storePath = argv[++i];
        } else if (arg == "--backend" && i + 1 < argc) {
            config.storeBackend = argv[++i];
            if (config.storeBackend == "postgresql" && !dbPortSet) {
                config.dbPort = 5432;  // 未显式指定端口时，PG 默认端口与 MySQL 不同
            }
        } else if (arg == "--mysql-host" && i + 1 < argc) {
            config.dbHost = argv[++i];
        } else if (arg == "--mysql-port" && i + 1 < argc) {
            config.dbPort = static_cast<std::uint16_t>(std::stoi(argv[++i]));
            dbPortSet = true;
        } else if (arg == "--mysql-user" && i + 1 < argc) {
            config.dbUser = argv[++i];
        } else if (arg == "--mysql-password" && i + 1 < argc) {
            config.dbPassword = argv[++i];
        } else if (arg == "--mysql-database" && i + 1 < argc) {
            config.dbDatabase = argv[++i];
        } else if (arg == "--pg-host" && i + 1 < argc) {
            config.dbHost = argv[++i];
        } else if (arg == "--pg-port" && i + 1 < argc) {
            config.dbPort = static_cast<std::uint16_t>(std::stoi(argv[++i]));
            dbPortSet = true;
        } else if (arg == "--pg-user" && i + 1 < argc) {
            config.dbUser = argv[++i];
        } else if (arg == "--pg-password" && i + 1 < argc) {
            config.dbPassword = argv[++i];
        } else if (arg == "--pg-database" && i + 1 < argc) {
            config.dbDatabase = argv[++i];
        } else if (arg == "--ops" && i + 1 < argc) {
            config.ops.enabled = std::stoi(argv[++i]) != 0;
        } else if (arg == "--ops-port" && i + 1 < argc) {
            config.ops.port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
        }
    }

    // 启动横幅在 move 前拼好——std::move(config) 之后源对象的字符串状态未定义
    std::ostringstream banner;
    banner << "DBApp listening on 0.0.0.0:" << config.listenPort
           << " backend=" << config.storeBackend;
    if (config.storeBackend == "mysql" || config.storeBackend == "postgresql") {
        banner << " (" << config.dbUser << "@" << config.dbHost
               << ":" << config.dbPort << "/" << config.dbDatabase << ")";
    } else {
        banner << " path=" << config.storePath;
    }

    theseed::db::DBApp app(std::move(config));

    if (!app.init()) {
        std::cerr << "DBApp init failed" << std::endl;
        return 1;
    }

    std::cout << banner.str() << std::endl;

    while (g_running) {
        app.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    app.stop();
    std::cout << "DBApp stopped." << std::endl;
    return 0;
}
