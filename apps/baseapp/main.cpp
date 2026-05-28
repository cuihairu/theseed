#include "theseed/core/BaseApp.h"
#include "theseed/core/FileEntityStore.h"
#include "theseed/runtime/NetworkTransport.h"
#include "theseed/runtime/TcpConnection.h"
#include "theseed/runtime/TickScheduler.h"
#include "theseed/runtime/TransportHub.h"

#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

static volatile bool g_running = true;

static void signalHandler(int) {
    g_running = false;
}

struct PeerConfig {
    theseed::runtime::ComponentId componentId;
    std::string host;
    std::uint16_t port;
};

int main(int argc, char** argv) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::string entityDefPath = "res/entities";
    std::string storePath = "data/entities";
    std::uint16_t clientPort = 20000;
    std::vector<PeerConfig> peers;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            clientPort = static_cast<std::uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--defs" && i + 1 < argc) {
            entityDefPath = argv[++i];
        } else if (arg == "--store" && i + 1 < argc) {
            storePath = argv[++i];
        } else if (arg == "--peer" && i + 2 < argc) {
            // --peer <componentId> <host:port>
            auto compId = static_cast<theseed::runtime::ComponentId>(std::stoi(argv[++i]));
            std::string addr = argv[++i];
            auto colon = addr.rfind(':');
            if (colon == std::string::npos) {
                std::cerr << "Invalid peer address: " << addr << std::endl;
                return 1;
            }
            peers.push_back({compId, addr.substr(0, colon),
                            static_cast<std::uint16_t>(std::stoi(addr.substr(colon + 1)))});
        }
    }

    auto hub = std::make_shared<theseed::runtime::TransportHub>(
        theseed::runtime::ComponentId{1});
    auto store = std::make_shared<theseed::core::FileEntityStore>(storePath);

    theseed::core::BaseApp::Config config;
    config.entityDefPath = entityDefPath;
    config.componentId = theseed::runtime::ComponentId{1};
    config.clientListenPort = clientPort;

    theseed::core::BaseApp app(std::move(config), hub, store);

    if (!app.init()) {
        std::cerr << "BaseApp init failed" << std::endl;
        return 1;
    }

    theseed::runtime::TickScheduler scheduler;
    app.attach(scheduler);

    // Connect to CellApp peers
    for (const auto& peer : peers) {
        auto conn = theseed::runtime::TcpConnection::create();
        if (!conn->connect(peer.host, peer.port)) {
            std::cerr << "Failed to connect to peer " << peer.componentId
                      << " at " << peer.host << ":" << peer.port << std::endl;
            continue;
        }
        auto transport = std::make_shared<theseed::runtime::NetworkTransport>(conn);
        hub->connectPeer(peer.componentId, transport);
        std::cout << "Connected to peer " << peer.componentId
                  << " at " << peer.host << ":" << peer.port << std::endl;
    }

    std::cout << "BaseApp listening on 0.0.0.0:" << clientPort << std::endl;

    while (g_running) {
        hub->tick();
        app.tick();
        scheduler.runOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    app.detach(scheduler);
    std::cout << "BaseApp stopped." << std::endl;
    return 0;
}
