#include "theseed/core/CellApp.h"
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

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--defs" && i + 1 < argc) {
            entityDefPath = argv[++i];
        }
    }

    auto transport = std::make_shared<theseed::runtime::PipedTransport>(
        theseed::runtime::ComponentId{2});

    theseed::core::CellApp::Config config;
    config.entityDefPath = entityDefPath;
    config.componentId = theseed::runtime::ComponentId{2};

    theseed::core::CellApp app(std::move(config), transport);

    if (!app.init()) {
        std::cerr << "CellApp init failed" << std::endl;
        return 1;
    }

    theseed::runtime::TickScheduler scheduler;
    app.attach(scheduler);

    std::cout << "CellApp running (component 2)" << std::endl;

    while (g_running) {
        scheduler.runOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    app.detach(scheduler);
    std::cout << "CellApp stopped." << std::endl;
    return 0;
}
