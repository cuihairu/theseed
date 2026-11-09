// theseed-bots 端到端测试：真实 TCP 回环上驱动 LoginApp + BaseApp/CellApp
// 全栈，BotManager 斜坡加压 → 登录 → 选区 → EnterGame → 移动/心跳 Action
// → 强制断线 → 退避重连恢复在线。
// 依赖真实 TCP 通信，macOS 上链路无法建立（与 LoginApp E2E 同款门控），
// 门控至 Linux。
#include "BotManager.h"
#include "BotsConfig.h"

#include "theseed/core/BaseApp.h"
#include "theseed/core/CellApp.h"
#include "theseed/db/IEntityStore.h"
#include "theseed/login/LoginApp.h"
#include "theseed/login/LoginTypes.h"
#include "theseed/runtime/NetworkNode.h"
#include "theseed/runtime/TcpConnection.h"
#include "theseed/runtime/TickScheduler.h"

#include <arpa/inet.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using theseed::bots::BotsConfig;
using theseed::bots::BotManager;
using theseed::bots::renderStatsLine;
using theseed::core::BaseApp;
using theseed::core::CellApp;
using theseed::core::InMemoryEntityStore;
using theseed::login::LoginApp;
using theseed::login::LoginAppConfig;
using theseed::login::RealmInfo;
using theseed::runtime::NetworkNode;
using theseed::runtime::TickScheduler;

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

namespace {

bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << content;
    return true;
}

std::string createDefDir() {
    const std::string dir = "test_bots_e2e_defs";
    std::filesystem::create_directory(dir);
    // 与 res/entities/Avatar.xml 同构：移动(Cell)/心跳(Base) 方法齐全，
    // bot 的 action 脚本能被 EntityDef 校验并分发。
    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="level" type="Int32"/>
        <Property name="hp" type="Float32"/>
    </Properties>
    <Methods>
        <Method name="onDamage" side="Cell"/>
        <Method name="onHeal" side="Base"/>
    </Methods>
</EntityDef>
)");
    return dir;
}

std::uint16_t freePort() {
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(s);
        return 0;
    }
    socklen_t len = sizeof(addr);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
    ::close(s);
    return ntohs(addr.sin_port);
}

// 驱动一轮回环：LoginApp tick + 集群 scheduler + bot 管理器。
void pumpOnce(LoginApp& loginApp, TickScheduler& scheduler, BotManager& mgr) {
    const auto now = std::chrono::steady_clock::now();
    loginApp.tick();
    scheduler.runOnce();
    mgr.tick(now);
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
}

// 反复泵直到 pred 成立或超时（毫秒）。
template <typename Pred>
bool pumpUntil(LoginApp& loginApp, TickScheduler& scheduler, BotManager& mgr,
               Pred pred, int maxMs) {
    for (int i = 0; i < maxMs; ++i) {
        pumpOnce(loginApp, scheduler, mgr);
        if (pred()) return true;
    }
    return false;
}

}  // namespace

int main() {
    theseed::runtime::TcpConnection::globalInit();

    const auto defDir = createDefDir();

    // --- BaseApp/CellApp 集群（与 tests/core ClientDisconnectTest 同形态） ---
    auto cellNode = std::make_shared<NetworkNode>(
        NetworkNode::Config{.localComponent = 2, .listenPort = 0});
    auto baseNode = std::make_shared<NetworkNode>(
        NetworkNode::Config{.localComponent = 1, .listenPort = 0});
    cellNode->setOnPeerConnected([cellNodePtr = cellNode.get()](
        std::shared_ptr<theseed::runtime::IRuntimeTransport> transport) {
        cellNodePtr->acceptPeer(1, transport);
    });

    auto store = std::make_shared<InMemoryEntityStore>();

    // clientListenPort=0：系统分配临时端口，避免与并行 ctest 的其它
    // BaseApp（缺省 20000）冲突。
    BaseApp baseApp(BaseApp::Config{.entityDefPath = defDir, .componentId = 1,
                                    .clientListenPort = 0},
                    baseNode->hub(), store);
    if (!baseApp.init()) {
        std::cout << "FAILED: BaseApp init" << std::endl;
        return 1;
    }
    CellApp cellApp(CellApp::Config{.entityDefPath = defDir, .componentId = 2},
                    cellNode->hub());
    if (!cellApp.init()) {
        std::cout << "FAILED: CellApp init" << std::endl;
        return 1;
    }
    baseNode->connectToPeer(2, "127.0.0.1", cellNode->listenPort());

    TickScheduler scheduler(std::chrono::milliseconds{4});
    baseNode->attach(scheduler);
    cellNode->attach(scheduler);
    baseApp.attach(scheduler);
    cellApp.attach(scheduler);

    // --- LoginApp：null 鉴权，realm 指向 BaseApp 客户端口 ---
    const std::uint16_t loginPort = freePort();
    LoginAppConfig loginConfig;
    loginConfig.listenPort = loginPort;
    loginConfig.authType = "null";
    RealmInfo realm;
    realm.realmId = "realm1";
    realm.name = "Test Realm";
    realm.status = "smooth";
    realm.host = "127.0.0.1";
    realm.port = baseApp.clientListenPort();
    loginConfig.realms.push_back(realm);

    LoginApp loginApp(loginConfig);
    loginApp.init();

    // --- bot 群：斜坡三件套 totalCount=3 / tickTime=20ms / tickCount=3 ---
    BotsConfig bots;
    bots.loginPort = loginPort;
    bots.realmId = "realm1";
    bots.totalCount = 3;
    bots.tickTime = std::chrono::milliseconds{20};
    bots.tickCount = 3;
    bots.moveInterval = std::chrono::milliseconds{5};
    bots.heartbeatInterval = std::chrono::milliseconds{10};
    bots.reconnectDelay = std::chrono::milliseconds{20};
    bots.statsInterval = std::chrono::milliseconds{3600000};  // 不干扰断言

    BotManager mgr(bots);

    const auto& agg = mgr.aggregate();

    std::cout << "bots E2E tests:" << std::endl;

    TEST("first ramp step spawns exactly one bot");
    {
        pumpOnce(loginApp, scheduler, mgr);
        if (agg.spawned != 1) FAIL("expected spawned=1 after first tick, got " + std::to_string(agg.spawned));
        else PASS();
    }

    TEST("ramp completes and all bots enter game");
    {
        if (!pumpUntil(loginApp, scheduler, mgr,
                       [&] { return agg.spawned == 3 && agg.inGame == 3; }, 8000)) {
            FAIL("bots did not reach InGame: " + renderStatsLine(agg));
        } else {
            PASS();
        }
    }

    TEST("bot traffic flows and RTT sampled");
    {
        // 已在线的 bot 持续发 move/heartbeat，等几个往返。
        pumpUntil(loginApp, scheduler, mgr,
                  [&] { return agg.messagesIn > 6 && agg.messagesOut > 6; }, 2000);
        if (agg.messagesIn <= 0 || agg.messagesOut <= 0) FAIL("no traffic");
        else if (agg.rttSamples == 0) FAIL("no RTT samples");
        else if (agg.protocolErrors != 0) FAIL("protocol errors: " + std::to_string(agg.protocolErrors));
        else PASS();
    }

    TEST("BaseApp holds one entity per online bot");
    {
        const auto count = baseApp.runtime().entityCount();
        if (count != 3) FAIL("entity count " + std::to_string(count) + " != 3");
        else PASS();
    }

    TEST("forced disconnect drops the bot and reconnect restores it");
    {
        mgr.disconnectBot(0, std::chrono::steady_clock::now());
        if (!pumpUntil(loginApp, scheduler, mgr,
                       [&] { return agg.inGame < 3; }, 3000)) {
            FAIL("disconnect did not take effect: " + renderStatsLine(agg));
        } else if (!pumpUntil(loginApp, scheduler, mgr,
                              [&] { return agg.inGame == 3 && agg.reconnects >= 1; }, 5000)) {
            FAIL("no reconnect recovery: " + renderStatsLine(agg));
        } else {
            PASS();
        }
    }

    // --- 第二群：严格斜坡节拍（每 100ms 1 个，共 4 个） ---
    TEST("strict ramp pacing (1 bot / 100ms up to 4)");
    {
        BotsConfig slow = bots;
        slow.totalCount = 4;
        slow.tickTime = std::chrono::milliseconds{100};
        slow.tickCount = 1;
        slow.moveInterval = std::chrono::milliseconds{50};
        BotManager mgr2(slow);
        // 首 tick 只出 1 个（斜坡不并发灌入）。
        pumpOnce(loginApp, scheduler, mgr2);
        const auto first = mgr2.aggregate().spawned;
        if (first != 1) FAIL("expected spawned=1 at t=0, got " + std::to_string(first));
        if (!pumpUntil(loginApp, scheduler, mgr2,
                       [&] { return mgr2.aggregate().inGame == 4; }, 10000)) {
            FAIL("slow ramp incomplete: " + renderStatsLine(mgr2.aggregate()));
        } else {
            PASS();
        }
        mgr2.stop();
    }

    std::cout << "final stats: " << renderStatsLine(agg) << std::endl;
    std::cout << "bots E2E: " << testsPassed << " passed, "
              << testsFailed << " failed" << std::endl;

    mgr.stop();
    baseApp.detach(scheduler);
    cellApp.detach(scheduler);
    // 节点也要显式注销：baseNode/cellNode 声明在 scheduler 之前，反序析构时
    // scheduler 已销毁，~NetworkNode 的 detach 会读到已释放的 tickable 向量
    //（ASan heap-use-after-free）。
    baseNode->detach(scheduler);
    cellNode->detach(scheduler);
    return testsFailed == 0 ? 0 : 1;
}
