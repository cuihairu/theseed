#include "theseed/core/BaseApp.h"
#include "theseed/core/FileEntityStore.h"
#include "theseed/runtime/PipedTransport.h"
#include "theseed/runtime/TickScheduler.h"

#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <thread>

static volatile bool g_running = true;

static void signalHandler(int) {
    g_running = false;
}

int main(int argc, char** argv) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::string entityDefPath = "res/entities";
    std::string storePath = "data/entities";
    std::uint16_t clientPort = 20000;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            clientPort = static_cast<std::uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--defs" && i + 1 < argc) {
            entityDefPath = argv[++i];
        } else if (arg == "--store" && i + 1 < argc) {
            storePath = argv[++i];
        }
    }

    auto transport = std::make_shared<theseed::runtime::PipedTransport>(
        theseed::runtime::ComponentId{1});
    auto store = std::make_shared<theseed::core::FileEntityStore>(storePath);

    theseed::core::BaseApp::Config config;
    config.entityDefPath = entityDefPath;
    config.componentId = theseed::runtime::ComponentId{1};
    config.clientListenPort = clientPort;

    theseed::core::BaseApp app(std::move(config), transport, store);

    if (!app.init()) {
        std::cerr << "BaseApp init failed" << std::endl;
        return 1;
    }

    theseed::runtime::TickScheduler scheduler;
    app.attach(scheduler);

    std::cout << "BaseApp listening on 0.0.0.0:" << clientPort << std::endl;

    while (g_running) {
        app.tick();
        scheduler.runOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    app.detach(scheduler);
    std::cout << "BaseApp stopped." << std::endl;
    return 0;
}
