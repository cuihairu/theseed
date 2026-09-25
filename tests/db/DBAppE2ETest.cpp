// DBApp 端到端测试：真实 TCP 回环上驱动 DBApp（file 后端）与客户端
// RemoteEntityStore / 裸协议请求，覆盖 init/tick/accept/全部 db.* 方法分发、
// 账号线性扫描回退、畸形载荷错误分支、OpsServer HTTP 端点与监听冲突。
// mysql/postgresql 后端场景按环境变量门控（与 store 集成测试同一套变量）。
#include "theseed/core/EntityData.h"
#include "theseed/db/DBApp.h"
#include "theseed/db/DBProtocol.h"
#include "theseed/db/RemoteEntityStore.h"
#include "theseed/runtime/NetworkTransport.h"
#include "theseed/runtime/TcpConnection.h"


#include <arpa/inet.h>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace theseed::db;
using theseed::core::DataType;
using theseed::core::EntityData;
using theseed::core::EntityId;
using theseed::core::PropertyData;
using theseed::runtime::ComponentId;
using theseed::runtime::NetworkTransport;
using theseed::runtime::RuntimeInvocation;
using theseed::runtime::SendResult;
using theseed::runtime::TcpConnection;

#define TEST(name)                     \
    do {                               \
        std::cout << "  " << name << "... "; \
    } while (0)
#define PASS() std::cout << "OK" << std::endl
#define FAIL(msg)                                    \
    do {                                             \
        std::cout << "FAILED: " << msg << std::endl; \
        return 1;                                    \
    } while (0)

namespace {

constexpr ComponentId kDbComponent = 10;
constexpr ComponentId kClientComponent = 1;  // DBApp 给首条入站连接分配的 peerId

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

EntityData makeAvatar(EntityId id, std::int32_t level) {
    EntityData data;
    data.id = id;
    data.entityType = "Avatar";
    PropertyData prop;
    prop.id = 0;
    prop.name = "level";
    prop.type = DataType::Int32;
    prop.rawValue = {std::byte{static_cast<unsigned char>(level)}, std::byte{0},
                     std::byte{0}, std::byte{0}};
    data.properties.push_back(prop);
    return data;
}

// 裸协议客户端：直连 DBApp 的监听端口，收发 RuntimeInvocation。
struct RawClient {
    std::shared_ptr<TcpConnection> conn;
    std::shared_ptr<NetworkTransport> transport;
    int pumpCount = 0;

    // pump 驱动服务端 tick 与客户端 socket 读取；带总量守卫防死循环挂死测试。
    void pump(const std::function<void()>& appTick) {
        if (++pumpCount > 20000) {
            std::cout << "FAILED: pump guard tripped (no response in 20000 pumps)"
                      << std::endl;
            std::exit(1);
        }
        appTick();
        transport->tick();
        usleep(500);
    }

    bool connect(std::uint16_t port) {
        conn = TcpConnection::create();
        if (!conn->connect("127.0.0.1", port)) return false;
        transport = std::make_shared<NetworkTransport>(conn);
        return true;
    }

    // 连接后等待服务端 accept 完成再发首包（connect 非阻塞，立即返回 true）。
    void settle(const std::function<void()>& appTick) {
        for (int i = 0; i < 40; ++i) {
            appTick();
            transport->tick();
            usleep(2000);
        }
        pumpCount = 0;
    }

    bool sendRequest(const std::string& method, std::vector<std::byte> payload) {
        RuntimeInvocation inv;
        inv.sourceComponent = kClientComponent;
        inv.targetComponent = kDbComponent;
        inv.method = method;
        inv.payload = std::move(payload);
        if (transport->send(std::move(inv)) != SendResult::Accepted) return false;
        transport->flush();
        return true;
    }

    // 发请求并泵到首条响应；超时返回 false。
    bool request(const std::string& method, std::vector<std::byte> payload,
                 const std::function<void()>& appTick, RuntimeInvocation& out) {
        if (!sendRequest(method, std::move(payload))) return false;
        for (int i = 0; i < 4000; ++i) {
            pump(appTick);
            if (transport->receive(kClientComponent, &out, 1) > 0) return true;
        }
        return false;
    }
};

// HTTP 探针：读取 OpsServer 响应的首行。accept/响应由应用 tick 驱动，
// 所以 recv 期间必须持续泵应用（非阻塞轮询，带总量上限）。
bool httpProbe(std::uint16_t port, const std::string& requestLine,
               std::string& firstLine, const std::function<void()>& pump) {
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(s);
        return false;
    }
    std::string req = requestLine + " HTTP/1.0\r\n\r\n";
    if (::send(s, req.data(), req.size(), 0) < 0) {
        ::close(s);
        return false;
    }
    char buf[4096] = {};
    ssize_t n = -1;
    for (int i = 0; i < 4000; ++i) {
        if (pump) pump();
        n = ::recv(s, buf, sizeof(buf) - 1, MSG_DONTWAIT);
        if (n > 0) break;
        usleep(500);
    }
    ::close(s);
    if (n <= 0) return false;
    firstLine = std::string(buf, static_cast<std::size_t>(n));
    return true;
}

}  // namespace

int main() {
    std::cout << "DBAppE2ETest:" << std::endl;

    const std::string storeDir = "test_dbapp_e2e_store";
    std::filesystem::remove_all(storeDir);

    // ------------------------------------------------------------------
    // 场景 1：file 后端 + ops 开启，全链路走通
    // ------------------------------------------------------------------
    std::uint16_t dbPort = freePort();
    std::uint16_t opsPort = freePort();
    if (dbPort == 0 || opsPort == 0) FAIL("cannot find free ports");

    DBApp::Config cfg;
    cfg.listenPort = dbPort;
    cfg.storePath = storeDir;
    cfg.storeBackend = "file";
    cfg.componentId = kDbComponent;
    cfg.ops.enabled = true;
    cfg.ops.port = opsPort;

    DBApp app(std::move(cfg));
    TEST("DBApp init (file backend + ops)");
    if (!app.init()) FAIL("DBApp::init failed");
    PASS();

    RawClient client;
    TEST("client connects over TCP");
    if (!client.connect(dbPort)) FAIL("client connect failed");
    auto appTick = [&app] { app.tick(); };
    client.settle(appTick);
    PASS();

    // --- RemoteEntityStore 全 API（file 后端）---
    {
        RemoteEntityStore store(client.transport, kDbComponent, kClientComponent);
        store.setPumpFunction([&client, &appTick] {
            // request() 内部无限泵，把守卫放进 pump 闭包
            if (++client.pumpCount > 20000) {
                std::cout << "FAILED: pump guard tripped in RemoteEntityStore"
                          << std::endl;
                std::exit(1);
            }
            appTick();
            client.transport->tick();
            usleep(500);
        });

        TEST("allocId returns 1 then 2 on fresh store");
        auto id1 = store.allocId();
        auto id2 = store.allocId();
        if (id1 != 1 || id2 != 2) {
            FAIL("allocId got " + std::to_string(id1) + "," + std::to_string(id2));
        }
        PASS();

        TEST("save + load round trip via RemoteEntityStore");
        if (!store.save(1, makeAvatar(1, 42))) FAIL("save avatar#1 failed");
        EntityData out;
        if (!store.load(1, "Avatar", out)) FAIL("load avatar#1 failed");
        bool levelOk = false;
        for (const auto& p : out.properties) {
            if (p.name == "level" && p.rawValue.size() == 4 &&
                p.rawValue[0] == std::byte{42}) {
                levelOk = true;
            }
        }
        if (!levelOk) FAIL("level property mismatch after round trip");
        PASS();

        TEST("load missing id returns false");
        if (store.load(999, "Avatar", out)) FAIL("load #999 should fail");
        PASS();

        TEST("listIdsByType + listEntityTypes");
        if (!store.save(2, makeAvatar(2, 7))) FAIL("save avatar#2 failed");
        auto ids = store.listIdsByType("Avatar");
        if (ids.size() != 2 || ids[0] != 1 || ids[1] != 2)
            FAIL("listIdsByType mismatch");
        auto types = store.listEntityTypes();
        bool hasAvatar = false;
        for (const auto& t : types) {
            if (t == "Avatar") hasAvatar = true;
        }
        if (!hasAvatar) FAIL("listEntityTypes missing Avatar");
        PASS();

        TEST("remove existing true / missing false");
        if (!store.remove(2)) FAIL("remove #2 should succeed");
        if (store.remove(999)) FAIL("remove #999 should fail");
        if (store.load(2, "Avatar", out)) FAIL("removed #2 still loads");
        PASS();
    }

    // --- 账号接口（file 后端无索引表 → 线性扫描回退分支）---
    {
        RuntimeInvocation resp;
        TEST("createAccount fallback creates account");
        if (!client.request(DBMethod::kCreateAccount,
                            DBProtocol::encodeCreateAccountRequest("alice", "pw1"),
                            appTick, resp))
            FAIL("no response to createAccount");
        bool ok = false;
        EntityId aliceId = 0;
        if (!DBProtocol::decodeCreateAccountResponse(
                std::span<const std::byte>(resp.payload.data(), resp.payload.size()),
                ok, aliceId))
            FAIL("decode createAccount response failed");
        if (!ok || aliceId == 0) FAIL("createAccount(alice) rejected");
        PASS();

        TEST("duplicate createAccount rejected via scan");
        if (!client.request(DBMethod::kCreateAccount,
                            DBProtocol::encodeCreateAccountRequest("alice", "x"),
                            appTick, resp))
            FAIL("no response to duplicate createAccount");
        if (!DBProtocol::decodeCreateAccountResponse(
                std::span<const std::byte>(resp.payload.data(), resp.payload.size()),
                ok, aliceId))
            FAIL("decode failed");
        if (ok) FAIL("duplicate username accepted");
        PASS();

        TEST("queryAccount fallback finds created account");
        if (!client.request(DBMethod::kQueryAccount,
                            DBProtocol::encodeQueryAccountRequest("alice"),
                            appTick, resp))
            FAIL("no response to queryAccount");
        bool found = false;
        EntityId qid = 0;
        std::string password;
        if (!DBProtocol::decodeQueryAccountResponse(
                std::span<const std::byte>(resp.payload.data(), resp.payload.size()),
                found, qid, password))
            FAIL("decode queryAccount response failed");
        if (!found || qid == 0 || password != "pw1") FAIL("account data mismatch");
        PASS();

        TEST("queryAccount miss returns not-found");
        if (!client.request(DBMethod::kQueryAccount,
                            DBProtocol::encodeQueryAccountRequest("nobody"),
                            appTick, resp))
            FAIL("no response to queryAccount(nobody)");
        if (!DBProtocol::decodeQueryAccountResponse(
                std::span<const std::byte>(resp.payload.data(), resp.payload.size()),
                found, qid, password))
            FAIL("decode failed");
        if (found) FAIL("unknown account reported found");
        PASS();
    }

    // --- 分支覆盖补充：查重扫描不匹配臂 / 缺 password 属性命中 / listIds 边界 ---
    {
        RuntimeInvocation resp;

        TEST("second account exercises scan-mismatch arms");
        // 创建 bob 时查重扫描遇到 alice：username 不匹配（storedName !=
        // username）与 password 属性（prop.name != "username"）两向分支。
        if (!client.request(DBMethod::kCreateAccount,
                            DBProtocol::encodeCreateAccountRequest("bob", "pw2"),
                            appTick, resp))
            FAIL("no response to createAccount(bob)");
        bool okBob = false;
        EntityId bobId = 0;
        if (!DBProtocol::decodeCreateAccountResponse(
                std::span<const std::byte>(resp.payload.data(), resp.payload.size()),
                okBob, bobId))
            FAIL("decode createAccount(bob) response failed");
        if (!okBob || bobId == 0) FAIL("createAccount(bob) rejected");
        PASS();

        TEST("queryAccount finds account without password property");
        // 手造只有 username 属性的 Account：命中后找 password 的循环自然
        // 走空退出，password 以空串返回。
        {
            RemoteEntityStore store(client.transport, kDbComponent, kClientComponent);
            store.setPumpFunction([&client, &appTick] {
                if (++client.pumpCount > 20000) {
                    std::cout << "FAILED: pump guard tripped" << std::endl;
                    std::exit(1);
                }
                appTick();
                client.transport->tick();
                usleep(500);
            });
            EntityData nameOnly;
            nameOnly.id = 901;
            nameOnly.entityType = "Account";
            PropertyData un;
            un.id = 0;
            un.name = "username";
            un.type = DataType::String;
            const std::string uname = "nopw_user";
            un.rawValue.resize(uname.size());
            std::memcpy(un.rawValue.data(), uname.data(), uname.size());
            nameOnly.properties.push_back(un);
            if (!store.save(901, nameOnly)) FAIL("save name-only account failed");
        }
        if (!client.request(DBMethod::kQueryAccount,
                            DBProtocol::encodeQueryAccountRequest("nopw_user"),
                            appTick, resp))
            FAIL("no response to queryAccount(nopw_user)");
        bool foundNp = false;
        EntityId npId = 0;
        std::string npPassword = "sentinel";
        if (!DBProtocol::decodeQueryAccountResponse(
                std::span<const std::byte>(resp.payload.data(), resp.payload.size()),
                foundNp, npId, npPassword))
            FAIL("decode queryAccount(nopw_user) response failed");
        if (!foundNp || npId != 901) FAIL("name-only account not found");
        if (!npPassword.empty()) FAIL("password should be empty without property");
        PASS();

        TEST("malformed listIds payload is tolerated");
        // len 字段撒谎（0xFFFFFFFF）：handleListIds 走越界防御，返回空列表。
        std::vector<std::byte> badListIds(4, std::byte{0xFF});
        if (!client.request(DBMethod::kListIds, badListIds, appTick, resp))
            FAIL("no response to malformed listIds");
        if (resp.method != DBMethod::kListIdsOk) FAIL("wrong response method");
        {
            std::vector<EntityId> outIds{1};
            DBProtocol::decodeListIdsResponse(
                std::span<const std::byte>(resp.payload.data(), resp.payload.size()),
                outIds);
            if (!outIds.empty()) FAIL("malformed listIds should yield empty list");
        }
        PASS();

        TEST("empty entityType listIds request");
        // len=0：handleListIds 跳过 memcpy，按空类型名查询。
        if (!client.request(DBMethod::kListIds,
                            DBProtocol::encodeListIdsRequest(""), appTick, resp))
            FAIL("no response to listIds(\"\")");
        if (resp.method != DBMethod::kListIdsOk) FAIL("wrong response method");
        PASS();
    }

    // --- 畸形载荷 → 错误响应分支 ---
    {
        std::vector<std::byte> garbage(64, std::byte{0xAB});
        RuntimeInvocation resp;

        TEST("malformed load/save/remove payloads get failure responses");
        if (!client.request(DBMethod::kLoad, garbage, appTick, resp))
            FAIL("no response to malformed load");
        if (resp.method != DBMethod::kLoadOk) FAIL("wrong response method");
        {
            bool ok = true;
            EntityData d;
            if (!DBProtocol::decodeLoadResponse(
                    std::span<const std::byte>(resp.payload.data(), resp.payload.size()),
                    ok, d) || ok)
                FAIL("malformed load should fail gracefully");
        }
        if (!client.request(DBMethod::kSave, garbage, appTick, resp))
            FAIL("no response to malformed save");
        if (resp.method != DBMethod::kSaveOk) FAIL("wrong response method");
        if (!client.request(DBMethod::kRemove, garbage, appTick, resp))
            FAIL("no response to malformed remove");
        if (resp.method != DBMethod::kRemoveOk) FAIL("wrong response method");
        PASS();

        TEST("malformed account payloads get not-found/failure responses");
        if (!client.request(DBMethod::kQueryAccount, garbage, appTick, resp))
            FAIL("no response to malformed queryAccount");
        if (resp.method != DBMethod::kQueryAccountOk) FAIL("wrong response method");
        if (!client.request(DBMethod::kCreateAccount, garbage, appTick, resp))
            FAIL("no response to malformed createAccount");
        if (resp.method != DBMethod::kCreateAccountOk) FAIL("wrong response method");
        PASS();

        TEST("short listIds payload and unknown method do not kill server");
        // 这两类请求服务端不回包；随后一条合法请求能正常应答即证明存活。
        client.sendRequest(DBMethod::kListIds, std::vector<std::byte>(2, std::byte{0}));
        client.sendRequest("db.nope", {});
        if (!client.request(DBMethod::kAllocId, {}, appTick, resp))
            FAIL("server unresponsive after junk requests");
        if (resp.method != DBMethod::kAllocIdOk) FAIL("wrong response method");
        PASS();
    }

    // --- OpsServer HTTP 端点 ---
    {
        auto opsTick = [&app] { app.tick(); };
        TEST("GET /metrics /health /inspect /entities return 200");
        for (const char* path : {"/metrics", "/health", "/inspect", "/entities"}) {
            std::string respLine;
            if (!httpProbe(opsPort, "GET " + std::string(path), respLine, opsTick))
                FAIL(std::string("no response from ") + path);
            if (respLine.find(" 200 ") == std::string::npos)
                FAIL(std::string(path) + " -> " + respLine.substr(0, 30));
        }
        PASS();

        TEST("unknown path 404 and non-GET 400");
        std::string respLine;
        if (!httpProbe(opsPort, "GET /nope", respLine, opsTick) ||
            respLine.find(" 404 ") == std::string::npos)
            FAIL("GET /nope should 404, got: " + respLine.substr(0, 30));
        if (!httpProbe(opsPort, "POST /metrics", respLine, opsTick) ||
            respLine.find(" 400 ") == std::string::npos)
            FAIL("POST should 400");
        PASS();
    }

    // --- 监听冲突：同端口第二个实例 init 失败 ---
    TEST("second DBApp on same port fails init");
    {
        DBApp::Config cfg2;
        cfg2.listenPort = dbPort;
        cfg2.storePath = storeDir + "_2";
        DBApp app2(std::move(cfg2));
        if (app2.init()) FAIL("second init on occupied port should fail");
    }
    PASS();

    // ------------------------------------------------------------------
    // 场景 2/3：mysql / postgresql 后端启动 + 一次往返（环境变量门控）
    // ------------------------------------------------------------------
    // lambda 不能复用 FAIL（其 return 1 会让返回类型推导成 int，正常路径
    // 落到函数末尾成为 UB，-O2/gcov 下直接 ud2）；改用返回 bool 的约定。
#define FAIL_SECTION(msg)                            \
    do {                                             \
        std::cout << "FAILED: " << msg << std::endl; \
        return false;                                \
    } while (0)
    auto sqlBackendSection = [&](const char* backend) -> bool {
        const std::string backendName(backend);
        TEST(backendName + " backend init fails on bad db port");
        {
            DBApp::Config badCfg;
            badCfg.listenPort = freePort();
            badCfg.storePath = storeDir + "_bad_" + backend;
            badCfg.storeBackend = backend;
            badCfg.dbPort = 1;  // 端口 1 无数据库服务
            DBApp badApp(std::move(badCfg));
            if (badApp.init()) FAIL_SECTION(backendName + "-backend init should fail");
        }
        PASS();

        TEST("DBApp boots with " + backendName + " backend");
        DBApp::Config scfg;
        scfg.listenPort = freePort();
        scfg.storePath = storeDir + "_" + backend;
        scfg.storeBackend = backend;

        const bool isMysql = std::string(backend) == "mysql";
        const char* envHost = std::getenv(isMysql ? "THESEED_MYSQL_HOST"
                                                  : "THESEED_PG_HOST");
        const char* envPort = std::getenv(isMysql ? "THESEED_MYSQL_PORT"
                                                  : "THESEED_PG_PORT");
        const char* envUser = std::getenv(isMysql ? "THESEED_MYSQL_USER"
                                                  : "THESEED_PG_USER");
        const char* envPw = std::getenv(isMysql ? "THESEED_MYSQL_PASSWORD"
                                                : "THESEED_PG_PASSWORD");
        const char* envDb = std::getenv(isMysql ? "THESEED_MYSQL_DATABASE"
                                                : "THESEED_PG_DATABASE");
        if (envHost) scfg.dbHost = envHost;
        if (envPort) scfg.dbPort = static_cast<std::uint16_t>(std::atoi(envPort));
        if (envUser) scfg.dbUser = envUser;
        if (envPw) scfg.dbPassword = envPw;
        if (envDb) scfg.dbDatabase = envDb;

        const std::uint16_t listenPort = scfg.listenPort;
        DBApp sapp(std::move(scfg));
        if (!sapp.init()) FAIL_SECTION(backendName + "-backend DBApp init failed");
        PASS();

        TEST(backendName + " backend serves allocId/save/load over TCP");
        RawClient sclient;
        if (!sclient.connect(listenPort))
            FAIL_SECTION("client connect to " + backendName + " DBApp failed");
        auto sappTick = [&sapp] { sapp.tick(); };
        sclient.settle(sappTick);

        RemoteEntityStore store(sclient.transport, kDbComponent, kClientComponent);
        store.setPumpFunction([&sclient, &sappTick, &backendName] {
            if (++sclient.pumpCount > 20000) {
                std::cout << "FAILED: pump guard tripped in " << backendName
                          << " section" << std::endl;
                std::exit(1);
            }
            sappTick();
            sclient.transport->tick();
            usleep(500);
        });

        auto id = store.allocId();
        if (id == 0) FAIL_SECTION("allocId returned 0");
        if (!store.save(id, makeAvatar(id, 5))) FAIL_SECTION("save failed");
        EntityData out;
        if (!store.load(id, "Avatar", out)) FAIL_SECTION("load failed");
        PASS();

        // --- accountStore_ 快路径：SQL 索引而非线性扫描 ---
        TEST(backendName + " backend account fast path (create/query/remove)");
        RuntimeInvocation resp;
        if (!sclient.request(DBMethod::kCreateAccount,
                             DBProtocol::encodeCreateAccountRequest("fast_user", "pw"),
                             sappTick, resp))
            FAIL_SECTION("no response to createAccount fast path");
        bool aok = false;
        EntityId accountId = 0;
        if (!DBProtocol::decodeCreateAccountResponse(
                std::span<const std::byte>(resp.payload.data(), resp.payload.size()),
                aok, accountId))
            FAIL_SECTION("decode createAccount response failed");
        if (!aok || accountId == 0) FAIL_SECTION("createAccount fast path rejected");

        if (!sclient.request(DBMethod::kQueryAccount,
                             DBProtocol::encodeQueryAccountRequest("fast_user"),
                             sappTick, resp))
            FAIL_SECTION("no response to queryAccount fast path");
        bool found = false;
        std::string fastPw;
        if (!DBProtocol::decodeQueryAccountResponse(
                std::span<const std::byte>(resp.payload.data(), resp.payload.size()),
                found, accountId, fastPw))
            FAIL_SECTION("decode queryAccount response failed");
        if (!found) FAIL_SECTION("queryAccount fast path not found");

        // 正常 remove 清掉账号与索引行——固定用户名的快路径测试必须自清理，
        // 否则下一次运行会因 username 唯一性被拒（flaky）。
        if (!sclient.request(DBMethod::kRemove,
                             DBProtocol::encodeRemoveRequest(accountId),
                             sappTick, resp))
            FAIL_SECTION("no response to account remove");
        bool removed = false;
        if (!DBProtocol::decodeRemoveResponse(
                std::span<const std::byte>(resp.payload.data(), resp.payload.size()),
                removed))
            FAIL_SECTION("decode remove response failed");
        if (!removed) FAIL_SECTION("account remove should succeed");

        if (!sclient.request(DBMethod::kQueryAccount,
                             DBProtocol::encodeQueryAccountRequest("fast_user"),
                             sappTick, resp))
            FAIL_SECTION("no response to post-remove queryAccount");
        if (!DBProtocol::decodeQueryAccountResponse(
                std::span<const std::byte>(resp.payload.data(), resp.payload.size()),
                found, accountId, fastPw))
            FAIL_SECTION("decode post-remove queryAccount response failed");
        if (found) FAIL_SECTION("account should be gone after remove");

        // handleRemove 畸形载荷：解码失败 → remove response(false)
        if (!sclient.request(DBMethod::kRemove, {std::byte{0xFF}, std::byte{0x00}},
                             sappTick, resp))
            FAIL_SECTION("no response to malformed remove");
        bool rok = true;
        if (!DBProtocol::decodeRemoveResponse(
                std::span<const std::byte>(resp.payload.data(), resp.payload.size()), rok))
            FAIL_SECTION("decode remove response failed");
        if (rok) FAIL_SECTION("malformed remove should report failure");
        PASS();
        return true;
    };
#undef FAIL_SECTION

    if (std::getenv("THESEED_MYSQL_HOST") != nullptr) {
        if (!sqlBackendSection("mysql")) return 1;
    }
    if (std::getenv("THESEED_PG_HOST") != nullptr) {
        if (!sqlBackendSection("postgresql")) return 1;
    }

    std::cout << "\nAll DBApp E2E tests passed!" << std::endl;
    return 0;
}
