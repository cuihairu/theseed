#include "theseed/core/CellApp.h"
#include "theseed/runtime/NetworkTransport.h"
#include "theseed/runtime/TcpConnection.h"
#include "theseed/runtime/TcpListener.h"
#include "theseed/runtime/TickScheduler.h"
#include "theseed/runtime/TransportHub.h"

#include "AppMainUtil.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    theseed::app::installSignalHandlers();

    std::string entityDefPath = "res/entities";
    std::uint16_t listenPort = 20002;
    bool opsEnabled = false;
    std::uint16_t opsPort = 20040;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            const auto parsed = theseed::app::parsePort(argv[++i]);
            if (!parsed) return 1;
            listenPort = *parsed;
        } else if (arg == "--defs" && i + 1 < argc) {
            entityDefPath = argv[++i];
        } else if (arg == "--ops" && i + 1 < argc) {
            const auto parsed = theseed::app::parseInt(argv[++i]);
            if (!parsed) return 1;
            opsEnabled = *parsed != 0;
        } else if (arg == "--ops-port" && i + 1 < argc) {
            const auto parsed = theseed::app::parsePort(argv[++i]);
            if (!parsed) return 1;
            opsPort = *parsed;
        }
    }

    auto hub = std::make_shared<theseed::runtime::TransportHub>(
        theseed::runtime::ComponentId{2});

    theseed::core::CellApp::Config config;
    config.entityDefPath = entityDefPath;
    config.componentId = theseed::runtime::ComponentId{2};
    config.ops.enabled = opsEnabled;
    config.ops.port = opsPort;

    theseed::core::CellApp app(std::move(config), hub);

    if (!app.init()) {
        std::cerr << "CellApp init failed" << std::endl;
        return 1;
    }

    theseed::runtime::TickScheduler scheduler;
    app.attach(scheduler);

    // Listen for server-to-server connections (BaseApp, other CellApps)
    theseed::runtime::TcpListener listener;
    listener.setConnectionFactory([]() {
        return theseed::runtime::TcpConnection::create();
    });
    listener.listen("0.0.0.0", listenPort);

    std::cout << "CellApp listening on 0.0.0.0:" << listenPort << std::endl;

    while (theseed::app::gRunning) {
        // Accept incoming peer connections
        while (auto conn = listener.accept()) {
            // Assign a component ID based on connection order
            // In production, this would use a handshake protocol
            static theseed::runtime::ComponentId nextPeer{1};
            auto peerId = nextPeer++;
            auto transport = std::make_shared<theseed::runtime::NetworkTransport>(conn);
            hub->connectPeer(peerId, transport);
            std::cout << "Peer " << peerId << " connected" << std::endl;
        }

        hub->tick();
        scheduler.runOnce();
        app.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    app.detach(scheduler);
    listener.close();
    std::cout << "CellApp stopped." << std::endl;
    return 0;
}
