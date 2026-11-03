#include "theseed/realm/RealmApp.h"
#include "theseed/runtime/TickScheduler.h"

#include "AppMainUtil.h"

#include <chrono>
#include <iostream>
#include <thread>

int main(int argc, char** argv) {
    theseed::app::installSignalHandlers();

    theseed::realm::RealmAppConfig config;
    config.listenHost = "0.0.0.0";
    config.listenPort = 20098;

    theseed::login::RealmInfo r1;
    r1.realmId = "east";
    r1.name = "East";
    r1.status = "smooth";
    r1.host = "127.0.0.1";
    r1.port = 20000;
    config.realms.push_back(r1);

    theseed::login::RealmInfo r2;
    r2.realmId = "south";
    r2.name = "South";
    r2.status = "smooth";
    r2.host = "127.0.0.1";
    r2.port = 20001;
    config.realms.push_back(r2);

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            const auto parsed = theseed::app::parsePort(argv[++i]);
            if (!parsed) return 1;
            config.listenPort = *parsed;
        } else if (arg == "--ops" && i + 1 < argc) {
            const auto parsed = theseed::app::parseInt(argv[++i]);
            if (!parsed) return 1;
            config.ops.enabled = *parsed != 0;
        } else if (arg == "--ops-port" && i + 1 < argc) {
            const auto parsed = theseed::app::parsePort(argv[++i]);
            if (!parsed) return 1;
            config.ops.port = *parsed;
        }
    }

    theseed::runtime::TickScheduler scheduler;
    theseed::realm::RealmApp app(std::move(config));

    app.init();

    std::cout << "RealmApp listening on " << config.listenHost << ":"
              << config.listenPort << std::endl;

    while (theseed::app::gRunning) {
        app.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    app.stop();
    std::cout << "RealmApp stopped." << std::endl;
    return 0;
}
