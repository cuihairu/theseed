#include "theseed/login/LoginApp.h"
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

    theseed::login::LoginAppConfig config;
    config.listenHost = "0.0.0.0";
    config.listenPort = 20099;
    config.authType = "null";

    theseed::login::RealmInfo realm;
    realm.realmId = "default";
    realm.name = "Default";
    realm.status = "smooth";
    realm.host = "127.0.0.1";
    realm.port = 20000;
    config.realms.push_back(realm);

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            config.listenPort = static_cast<std::uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--auth" && i + 1 < argc) {
            config.authType = argv[++i];
        }
    }

    theseed::runtime::TickScheduler scheduler;
    theseed::login::LoginApp app(std::move(config));

    app.init();

    std::cout << "LoginApp listening on " << config.listenHost << ":"
              << config.listenPort << std::endl;

    while (g_running) {
        app.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    app.stop();
    std::cout << "LoginApp stopped." << std::endl;
    return 0;
}
