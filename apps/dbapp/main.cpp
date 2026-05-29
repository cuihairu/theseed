#include "theseed/db/DBApp.h"
#include "theseed/runtime/TickScheduler.h"

#include <chrono>
#include <csignal>
#include <iostream>
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

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            config.listenPort = static_cast<std::uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--store" && i + 1 < argc) {
            config.storePath = argv[++i];
        }
    }

    theseed::db::DBApp app(std::move(config));

    if (!app.init()) {
        std::cerr << "DBApp init failed" << std::endl;
        return 1;
    }

    std::cout << "DBApp listening on 0.0.0.0:" << config.listenPort << std::endl;

    while (g_running) {
        app.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    app.stop();
    std::cout << "DBApp stopped." << std::endl;
    return 0;
}
