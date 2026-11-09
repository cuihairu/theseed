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
            // 服务端身份自注册：对端身份由其首条入站消息的 sourceComponent
            // 自报（TransportHub::receive 自学习），回复按 target=sourceComponent
            // 路由回去。不再按连接顺序手工分配 component ID——旧法以本地顺序
            // 号冒充对端身份，与对端自报不一致时回复被 hub 静默丢弃。
            auto transport = std::make_shared<theseed::runtime::NetworkTransport>(conn);
            hub->attachServerTransport(transport);
            std::cout << "Peer connected (identity self-reported)" << std::endl;
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
