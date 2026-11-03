// BotsConfig 单元测试：key=value 解析、默认值、范围校验、未知/重复键拒绝。
#include "BotsConfig.h"

#include <iostream>
#include <string>

using theseed::bots::BotsConfig;
using theseed::bots::parseBotsConfig;
using theseed::bots::loadBotsConfig;

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name)                                           \
    do {                                                     \
        std::cout << "  " << (name) << "... " << std::flush; \
    } while (0)

#define PASS()                        \
    do {                              \
        std::cout << "OK\n";          \
        ++testsPassed;                \
    } while (0)

#define FAIL(msg)                                  \
    do {                                           \
        std::cout << "FAILED: " << (msg) << "\n";  \
        ++testsFailed;                             \
    } while (0)

int main() {
    std::cout << "BotsConfig tests:" << std::endl;

    TEST("empty text keeps defaults");
    {
        BotsConfig cfg;
        std::string err;
        if (!parseBotsConfig("", cfg, err)) FAIL(err);
        if (cfg.loginHost != "127.0.0.1" || cfg.loginPort != 20099) FAIL("login default");
        if (cfg.totalCount != 10 || cfg.tickCount != 1) FAIL("ramp default");
        if (cfg.tickTime != std::chrono::milliseconds{1000}) FAIL("tickTime default");
        if (cfg.realmId != "default" || cfg.accountPrefix != "bot_") FAIL("account default");
        if (cfg.basePort != 0 || !cfg.baseHost.empty()) FAIL("base override default");
    }
    PASS();

    TEST("full custom config parses");
    {
        BotsConfig cfg;
        std::string err;
        const std::string text = R"(
# 斜坡压测配置
login_host = 10.0.0.5
login_port = 30099
base_host = 10.0.0.6   ; 行内注释也生效
base_port = 30000
realm_id = realm_east
account_prefix = bot_x_
account_password = secret
total_count = 50
tick_time = 500
tick_count = 5
move_interval = 100
heartbeat_interval = 250
move_action = onDamage
heartbeat_action = onHeal
reconnect_delay = 1500
stats_interval = 2000
run_time = 60
tick_interval = 5
)";
        if (!parseBotsConfig(text, cfg, err)) FAIL(err);
        if (cfg.loginHost != "10.0.0.5" || cfg.loginPort != 30099) FAIL("login");
        if (cfg.baseHost != "10.0.0.6" || cfg.basePort != 30000) FAIL("base");
        if (cfg.realmId != "realm_east") FAIL("realm");
        if (cfg.accountPrefix != "bot_x_" || cfg.accountPassword != "secret") FAIL("account");
        if (cfg.totalCount != 50 || cfg.tickCount != 5) FAIL("ramp counts");
        if (cfg.tickTime != std::chrono::milliseconds{500}) FAIL("tickTime");
        if (cfg.moveInterval != std::chrono::milliseconds{100}) FAIL("moveInterval");
        if (cfg.heartbeatInterval != std::chrono::milliseconds{250}) FAIL("heartbeatInterval");
        if (cfg.moveAction != "onDamage" || cfg.heartbeatAction != "onHeal") FAIL("actions");
        if (cfg.reconnectDelay != std::chrono::milliseconds{1500}) FAIL("reconnect");
        if (cfg.statsInterval != std::chrono::milliseconds{2000}) FAIL("stats");
        if (cfg.runTime != std::chrono::seconds{60}) FAIL("runTime");
        if (cfg.tickInterval != std::chrono::milliseconds{5}) FAIL("tickInterval");
    }
    PASS();

    TEST("unknown key rejected");
    {
        BotsConfig cfg;
        std::string err;
        if (parseBotsConfig("totl_count = 5", cfg, err)) FAIL("should reject typo key");
        if (err.find("totl_count") == std::string::npos) FAIL("error should name the key");
    }
    PASS();

    TEST("duplicate key rejected");
    {
        BotsConfig cfg;
        std::string err;
        if (parseBotsConfig("total_count = 5\ntotal_count = 6", cfg, err))
            FAIL("should reject duplicate");
        if (err.find("duplicate") == std::string::npos) FAIL("error should say duplicate");
    }
    PASS();

    TEST("non-integer and out-of-range values rejected");
    {
        BotsConfig cfg;
        std::string err;
        if (parseBotsConfig("total_count = abc", cfg, err)) FAIL("should reject abc");
        if (parseBotsConfig("login_port = 0", cfg, err)) FAIL("should reject port 0");
        if (parseBotsConfig("login_port = 70000", cfg, err)) FAIL("should reject port 70000");
        if (parseBotsConfig("tick_time = -5", cfg, err)) FAIL("should reject negative ms");
        if (parseBotsConfig("run_time = 99999999", cfg, err)) FAIL("should reject huge run_time");
    }
    PASS();

    TEST("missing file rejected");
    {
        BotsConfig cfg;
        std::string err;
        if (loadBotsConfig("/nonexistent/bots.ini", cfg, err)) FAIL("should fail");
        if (err.find("cannot open") == std::string::npos) FAIL("error should say cannot open");
    }
    PASS();

    std::cout << "BotsConfig: " << testsPassed << " passed, "
              << testsFailed << " failed" << std::endl;
    return testsFailed == 0 ? 0 : 1;
}
