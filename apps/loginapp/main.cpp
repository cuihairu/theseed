#include "theseed/foundation/RateLimiter.h"
#include "theseed/foundation/RedisProvider.h"
#include "theseed/foundation/SessionStore.h"
#include "theseed/login/LoginApp.h"
#include "theseed/runtime/TickScheduler.h"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <string>
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

    // Redis 会话/限流默认开启（内存实现）。生产环境替换为 hiredis 后端即可。
    bool enableRedis = true;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            config.listenPort = static_cast<std::uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--auth" && i + 1 < argc) {
            config.authType = argv[++i];
        } else if (arg == "--no-redis") {
            enableRedis = false;
        } else if (arg == "--machine-host" && i + 1 < argc) {
            // 控制面通知腿（04 §8 踢人联动）：出站连 MachineDaemon，
            // 接收 machine.session.revoked 并关掉匹配的活跃登录连接。
            // 不给即不接线（缺省安全：联动是增强，不是登录前提）。
            config.machineHost = argv[++i];
        } else if (arg == "--machine-port" && i + 1 < argc) {
            config.machinePort = static_cast<std::uint16_t>(std::stoi(argv[++i]));
        }
    }

    if (enableRedis) {
        auto redis = std::make_shared<theseed::foundation::InMemoryRedisProvider>();
        config.redis = redis;
        config.sessionStore = std::make_shared<theseed::foundation::SessionStore>(redis);
        config.rateLimiter = std::make_shared<theseed::foundation::RateLimiter>(redis);
        // 默认：每账号每秒 5 次登录尝试上限 10。
        config.rateLimitConfig.capacity = 10;
        config.rateLimitConfig.refillInterval = std::chrono::milliseconds(200);
    }

    theseed::runtime::TickScheduler scheduler;
    // 打印用的监听信息在 move 之前取好（config 移动后其字符串成员为空）。
    const std::string listenHost = config.listenHost;
    const std::uint16_t listenPort = config.listenPort;
    const bool machineLink = !config.machineHost.empty();
    theseed::login::LoginApp app(std::move(config));

    app.init();

    std::cout << "LoginApp listening on " << listenHost << ":" << listenPort
              << " redis=" << (enableRedis ? "on" : "off")
              << " machine-link=" << (machineLink ? "on" : "off")
              << std::endl;

    while (g_running) {
        app.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    app.stop();
    std::cout << "LoginApp stopped." << std::endl;
    return 0;
}
