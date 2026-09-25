// OpsServer 单元测试：真实回环 HTTP 驱动全部端点与请求解析错误分支
// （无头部长度的超大请求、不完整请求、非 GET 方法、query 剥离、
// 空闲连接与对端断开后的清理）。
#include "theseed/ops/OpsServer.h"

#include "theseed/runtime/TcpConnection.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <vector>

using theseed::ops::OpsInspector;
using theseed::ops::OpsServer;
using theseed::runtime::TcpConnection;

namespace {

int gFailures = 0;

#define CHECK(cond, msg)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            std::cout << "  FAILED: " << msg << std::endl;   \
            ++gFailures;                                     \
        }                                                    \
    } while (0)

struct HttpReply {
    int status = 0;
    std::string body;
    std::string contentType;
};

void parseReply(const std::string& raw, HttpReply& out) {
    const auto eoh = raw.find("\r\n\r\n");
    if (eoh == std::string::npos) return;
    const std::string head = raw.substr(0, eoh);
    out.body = raw.substr(eoh + 4);
    if (head.compare(0, 12, "HTTP/1.0 200") == 0) out.status = 200;
    else if (head.compare(0, 12, "HTTP/1.0 400") == 0) out.status = 400;
    else if (head.compare(0, 12, "HTTP/1.0 404") == 0) out.status = 404;
    const auto ct = head.find("Content-Type: ");
    if (ct != std::string::npos) {
        const auto end = head.find("\r\n", ct);
        out.contentType = head.substr(ct + 14, end == std::string::npos
                                                ? std::string::npos : end - ct - 14);
    }
}

// 建一条连接发送原始请求，tick 服务端 + pump 客户端直到收到响应。
HttpReply sendRequest(OpsServer& server, std::uint16_t port,
                      const std::string& request, int rounds = 200) {
    HttpReply reply;
    std::string rx;
    auto conn = TcpConnection::create();
    conn->setOnReceived([&rx](std::span<const std::byte> data) {
        rx.append(reinterpret_cast<const char*>(data.data()), data.size());
    });
    if (!conn->connect("127.0.0.1", port)) {
        std::cout << "  FAILED: connect for " << request.substr(0, request.find("\r\n"))
                  << std::endl;
        ++gFailures;
        return reply;
    }
    conn->write(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(request.data()), request.size()));
    for (int i = 0; i < rounds && rx.empty(); ++i) {
        server.tick();
        conn->pump();
    }
    parseReply(rx, reply);
    return reply;
}

}  // namespace

int main() {
    TcpConnection::globalInit();

    theseed::ops::ProcessInfo info;
    info.role = "TestApp";
    OpsInspector inspector(info);

    OpsServer::Config cfg;
    cfg.port = 0;  // ephemeral
    OpsServer server(cfg, inspector);

    CHECK(!server.isListening(), "not listening before start");
    CHECK(server.start(), "start");
    CHECK(server.isListening(), "listening after start");
    CHECK(server.localPort() != 0, "localPort assigned");
    const auto port = server.localPort();

    // 四个端点
    {
        auto r = sendRequest(server, port, "GET /metrics HTTP/1.1\r\n\r\n");
        CHECK(r.status == 200, "metrics 200");
        CHECK(r.contentType.find("text/plain") != std::string::npos, "metrics content-type");
    }
    {
        auto r = sendRequest(server, port, "GET /health HTTP/1.1\r\n\r\n");
        CHECK(r.status == 200, "health 200");
        CHECK(r.body.find("TestApp") != std::string::npos, "health has role");
    }
    {
        auto r = sendRequest(server, port, "GET /inspect HTTP/1.1\r\n\r\n");
        CHECK(r.status == 200 && r.body.find("\"role\"") != std::string::npos, "inspect 200 json");
    }
    {
        auto r = sendRequest(server, port, "GET /entities HTTP/1.1\r\n\r\n");
        CHECK(r.status == 200 && r.body.find("entity") != std::string::npos, "entities 200 json");
    }

    // query 剥离：/inspect?x=1 等价 /inspect
    {
        auto r = sendRequest(server, port, "GET /inspect?pretty=1 HTTP/1.1\r\n\r\n");
        CHECK(r.status == 200 && r.body.find("\"role\"") != std::string::npos,
              "query string stripped");
    }

    // 非 GET：POST → 400
    {
        auto r = sendRequest(server, port, "POST /inspect HTTP/1.1\r\n\r\n");
        CHECK(r.status == 400, "POST rejected");
    }

    // 缺少路径空格 → 400
    {
        auto r = sendRequest(server, port, "GARBAGE\r\n\r\n");
        CHECK(r.status == 400, "malformed request line rejected");
    }

    // 不完整请求（无头部终止符）：先 tick 等待，补齐后正常响应
    {
        auto c = TcpConnection::create();
        CHECK(c->connect("127.0.0.1", port), "connect partial");
        c->write(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>("GET /health HTTP/1.1\r\n"), 21));
        for (int i = 0; i < 3; ++i) {
            server.tick();
            c->pump();
        }
        c->write(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>("\r\n\r\n"), 4));
        std::string rx;
        c->setOnReceived([&rx](std::span<const std::byte> data) {
            rx.append(reinterpret_cast<const char*>(data.data()), data.size());
        });
        for (int i = 0; i < 200 && rx.empty(); ++i) {
            server.tick();
            c->pump();
        }
        HttpReply r;
        parseReply(rx, r);
        CHECK(r.status == 200, "partial request completed");
    }

    // 超过 8KB 仍无头部终止符：直接 400
    {
        auto r = sendRequest(server, port, std::string(9000, 'x') + "\r\n");
        CHECK(r.status == 400, "oversized headerless request rejected");
    }

    // 空闲连接（不发数据）与对端断开：均不应影响后续请求
    {
        auto idle = TcpConnection::create();
        CHECK(idle->connect("127.0.0.1", port), "connect idle");
        for (int i = 0; i < 3; ++i) {
            server.tick();
            idle->pump();
        }
        idle->close();
        server.tick();

        auto r = sendRequest(server, port, "GET /health HTTP/1.1\r\n\r\n");
        CHECK(r.status == 200, "server healthy after idle/dropped connections");
    }

    // 头部以 \n\n 终止（eoh2 独立命中路径）
    {
        auto r = sendRequest(server, port, "GET /health HTTP/1.1\n\n");
        CHECK(r.status == 200, "LF-only header terminator accepted");
    }

    // 请求行无空格（sp1 缺失）→ 400
    {
        auto r = sendRequest(server, port, "PING\r\n\r\n");
        CHECK(r.status == 400, "request line without spaces rejected");
    }

    // 请求行仅一个空格（sp1 命中、sp2 缺失）→ 400
    {
        auto r = sendRequest(server, port, "GET /onlypath\r\n\r\n");
        CHECK(r.status == 400, "request line with single space rejected");
    }

    // maxConnections=0：accept 循环不进（while 假臂），请求得不到服务
    {
        OpsServer::Config cfg0;
        cfg0.port = 0;
        cfg0.maxConnections = 0;
        OpsServer server0(cfg0, inspector);
        CHECK(server0.start(), "start zero-conn server");
        auto r = sendRequest(server0, server0.localPort(), "GET /health HTTP/1.1\r\n\r\n", 20);
        CHECK(r.status == 0, "zero maxConnections serves nothing");
        server0.stop();
    }

    // stop 后不再监听；重复 stop 安全
    server.stop();
    CHECK(!server.isListening(), "not listening after stop");
    server.stop();

    TcpConnection::globalShutdown();

    if (gFailures == 0) {
        std::cout << "OpsServerTest: all passed" << std::endl;
    } else {
        std::cout << "OpsServerTest: " << gFailures << " failure(s)" << std::endl;
    }
    return gFailures == 0 ? 0 : 1;
}
