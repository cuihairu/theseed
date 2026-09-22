// RealmApp 端到端测试：真实 TCP 回环驱动完整 tick 循环——init(ops 开启)、
// accept、QueryRealms 请求/应答、未知消息类型存活、ops 端点、断开清理。
#include "theseed/foundation/MemoryStream.h"
#include "theseed/login/LoginProtocol.h"
#include "theseed/login/LoginTypes.h"
#include "theseed/realm/RealmApp.h"
#include "theseed/runtime/TcpConnection.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace theseed;
using namespace theseed::realm;
using theseed::login::ClientMessageType;
using theseed::login::LoginProtocol;
using theseed::login::RealmInfo;
using theseed::runtime::TcpConnection;

#define TEST(name)                            \
    do {                                      \
        std::cout << "  " << name << "... ";  \
    } while (0)
#define PASS() std::cout << "OK" << std::endl
#define FAIL(msg)                                    \
    do {                                             \
        std::cout << "FAILED: " << msg << std::endl; \
        return 1;                                    \
    } while (0)

namespace {

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

// 裸帧客户端：直接收发 LoginProtocol 帧。
struct FrameClient {
    std::shared_ptr<TcpConnection> conn;
    std::vector<std::byte> inbox;
    int pumpCount = 0;

    bool connect(std::uint16_t port) {
        conn = TcpConnection::create();
        conn->setOnReceived([this](std::span<const std::byte> data) {
            inbox.insert(inbox.end(), data.begin(), data.end());
        });
        return conn->connect("127.0.0.1", port);
    }

    bool send(ClientMessageType type, std::span<const std::byte> payload) {
        auto frame = LoginProtocol::frameMessage(type, payload);
        if (!conn->write(std::span<const std::byte>(frame.data(), frame.size())))
            return false;
        conn->pump();
        return true;
    }

    // 泵应用 tick 与客户端 socket，直到收齐一帧或超时。
    bool recvFrame(const std::function<void()>& appTick,
                   ClientMessageType& outType, std::vector<std::byte>& outPayload) {
        for (int i = 0; i < 4000; ++i) {
            if (++pumpCount > 20000) {
                std::cout << "FAILED: pump guard tripped" << std::endl;
                std::exit(1);
            }
            appTick();
            conn->pump();
            ClientMessageType type;
            std::span<const std::byte> payload;
            if (LoginProtocol::parseFrame(std::span<const std::byte>(inbox.data(), inbox.size()),
                                          type, payload)) {
                outType = type;
                outPayload.assign(payload.begin(), payload.end());
                inbox.clear();
                return true;
            }
            usleep(500);
        }
        return false;
    }
};

// 从 encodeRealmList 的 payload 里解出第一个 realm（逆着 writeString/writeUint16）。
RealmInfo decodeFirstRealm(std::span<const std::byte> payload) {
    foundation::MemoryStream ms;
    ms.writeBytes(payload.data(), payload.size());
    ms.resetRead();  // 读写指针分离，重置读指针后开始解析
    const auto count = ms.readUint32();
    RealmInfo r{};
    if (count == 0) return r;
    const auto readStr = [&ms] {
        const auto len = ms.readUint32();
        std::string s(len, '\0');
        if (len > 0) ms.readBytes(s.data(), len);
        return s;
    };
    r.realmId = readStr();
    r.name = readStr();
    r.status = readStr();
    r.host = readStr();
    r.port = ms.readUint16();
    return r;
}

// HTTP 探针：非阻塞轮询 recv，泵应用 tick 驱动 accept/响应。
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
    std::cout << "RealmAppE2ETest:" << std::endl;

    const std::uint16_t listenPort = freePort();
    const std::uint16_t opsPort = freePort();
    if (listenPort == 0 || opsPort == 0) FAIL("cannot find free ports");

    RealmAppConfig cfg;
    cfg.listenHost = "127.0.0.1";
    cfg.listenPort = listenPort;
    cfg.realms.push_back(RealmInfo{"realm1", "测试区", "online", "127.0.0.1", 30001});
    cfg.ops.enabled = true;
    cfg.ops.port = opsPort;

    RealmApp app(std::move(cfg));
    TEST("init with ops enabled");
    app.init();
    PASS();

    TEST("realms() returns configured list");
    if (app.realms().size() != 1 || app.realms()[0].realmId != "realm1")
        FAIL("realms() mismatch");
    PASS();

    FrameClient client;
    TEST("client connects over TCP");
    if (!client.connect(listenPort)) FAIL("client connect failed");
    auto appTick = [&app] { app.tick(); };
    for (int i = 0; i < 40; ++i) {  // 等 accept
        appTick();
        client.conn->pump();
        usleep(2000);
    }
    PASS();

    TEST("QueryRealms round trip returns configured realm");
    if (!client.send(ClientMessageType::QueryRealms, {}))
        FAIL("send QueryRealms failed");
    ClientMessageType type;
    std::vector<std::byte> payload;
    if (!client.recvFrame(appTick, type, payload))
        FAIL("no response to QueryRealms");
    if (type != ClientMessageType::QueryRealmsResponse)
        FAIL("wrong response type");
    const auto realm = decodeFirstRealm(payload);
    if (realm.realmId != "realm1" || realm.name != "测试区" || realm.status != "online" ||
        realm.host != "127.0.0.1" || realm.port != 30001)
        FAIL("realm data mismatch");
    PASS();

    TEST("unknown message type does not kill server");
    std::vector<std::byte> junk(4, std::byte{0});
    if (!client.send(static_cast<ClientMessageType>(99), junk))
        FAIL("send junk frame failed");
    if (!client.send(ClientMessageType::QueryRealms, {}))
        FAIL("connection broken after junk frame");
    if (!client.recvFrame(appTick, type, payload))
        FAIL("server unresponsive after junk frame");
    if (type != ClientMessageType::QueryRealmsResponse)
        FAIL("wrong response type after junk frame");
    PASS();

    TEST("ops /health and /inspect return 200");
    {
        auto opsTick = [&app] { app.tick(); };
        for (const char* path : {"/health", "/inspect"}) {
            std::string respLine;
            if (!httpProbe(opsPort, "GET " + std::string(path), respLine, opsTick))
                FAIL(std::string("no response from ") + path);
            if (respLine.find(" 200 ") == std::string::npos)
                FAIL(std::string(path) + " -> " + respLine.substr(0, 30));
        }
        PASS();
    }

    TEST("disconnect cleans up session");
    {
        client.conn->close();
        auto opsTick = [&app] { app.tick(); };
        // 泵若干 tick 让服务端感知断开并触发 cleanupDisconnected。
        bool removed = false;
        for (int i = 0; i < 200 && !removed; ++i) {
            opsTick();
            usleep(2000);
            std::string respLine;
            if (httpProbe(opsPort, "GET /inspect", respLine, opsTick) &&
                respLine.find("\"session_count\":0") != std::string::npos)
                removed = true;
        }
        if (!removed) FAIL("session not cleaned up after disconnect");
        PASS();
    }

    std::cout << "\nAll RealmApp E2E tests passed!" << std::endl;
    return 0;
}
