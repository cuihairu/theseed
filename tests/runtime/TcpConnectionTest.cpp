// TcpConnection 回环测试：连接拒绝、echo 往返、内核缓冲打满后的 EAGAIN
// 路径、对端关闭感知与重复 close 安全性。全部单线程驱动（两端都 pump）。
#include "theseed/runtime/TcpConnection.h"
#include "theseed/runtime/TcpListener.h"
#include "SocketDetail.h"  // TU 私有胶合头（测试目标私有 include）：connectStillPending 纯函数直测

#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <iostream>
#include <span>
#include <string>
#include <thread>
#include <vector>

using theseed::runtime::TcpConnection;
using theseed::runtime::TcpListener;

namespace {

int gFailures = 0;

#define CHECK(cond, msg)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            std::cout << "  FAILED: " << msg << std::endl;   \
            ++gFailures;                                     \
        }                                                    \
    } while (0)

}  // namespace

int main() {
    TcpConnection::globalInit();

    // 未连接的 write 拒绝；同步拒绝路径（非 in-progress 分支）
    {
        auto c = TcpConnection::create();
        std::byte b{0x01};
        CHECK(!c->write(std::span<const std::byte>(&b, 1)), "write before connect");
#ifndef _WIN32
        // 广播地址不可路由：非阻塞 connect 立即返回错误（不走 in-progress）
        CHECK(!c->connect("255.255.255.255", 80), "connect unroutable fails");
        CHECK(!c->isConnected(), "not connected after failed connect");
#endif
    }

    // echo 往返 + 对端关闭感知 + 重复 close
    {
        TcpListener listener;
        CHECK(listener.listen("127.0.0.1", 0), "listen");
        const auto port = listener.localPort();
        CHECK(listener.isListening(), "listening");

        auto client = TcpConnection::create();
        CHECK(client->connect("127.0.0.1", port), "client connect");
        // 已连接状态下二次 connect 被拒绝
        CHECK(!client->connect("127.0.0.1", port), "connect while connected rejected");

        auto server = listener.accept();
        CHECK(server != nullptr, "accept");

        std::string serverRx;
        server->setOnReceived([&server, &serverRx](std::span<const std::byte> data) {
            serverRx.append(reinterpret_cast<const char*>(data.data()), data.size());
            // echo 回去
            server->write(data);
        });

        std::string clientRx;
        client->setOnReceived([&clientRx](std::span<const std::byte> data) {
            clientRx.append(reinterpret_cast<const char*>(data.data()), data.size());
        });

        const std::string payload = "ping-echo-123";
        CHECK(client->write(std::span<const std::byte>(
                  reinterpret_cast<const std::byte*>(payload.data()), payload.size())),
              "write payload");
        for (int i = 0; i < 200 && clientRx.size() < payload.size(); ++i) {
            client->pump();
            server->pump();
        }
        CHECK(clientRx == payload, "echo roundtrip");

        // 服务器端关闭：客户端 pump 收到 EOF → isConnected 变 false
        server->close();
        for (int i = 0; i < 200 && client->isConnected(); ++i) {
            client->pump();
        }
        CHECK(!client->isConnected(), "client sees EOF after server close");
        client->close();
        client->close();  // 双重 close 安全
    }

    // 收到数据但未设置回调：recv 正常消费并统计字节，只是不投递（onReceived_ 空臂）。
    {
        TcpListener listener;
        CHECK(listener.listen("127.0.0.1", 0), "listen");
        auto client = TcpConnection::create();
        CHECK(client->connect("127.0.0.1", listener.localPort()), "client connect");
        auto server = listener.accept();
        CHECK(server != nullptr, "accept");

        const std::string payload = "no-callback";
        CHECK(client->write(std::span<const std::byte>(
                  reinterpret_cast<const std::byte*>(payload.data()), payload.size())),
              "write payload");
        bool consumed = false;
        for (int i = 0; i < 200 && !consumed; ++i) {
            consumed = server->pumpWithResult() > 0;
        }
        CHECK(consumed, "recv consumed bytes without callback");
        client->close();
        server->close();
    }

    // 写满内核发送缓冲：send 返回 EAGAIN 走 wouldBlock 分支
    {
        TcpListener listener;
        CHECK(listener.listen("127.0.0.1", 0), "listen flood");
        auto client = TcpConnection::create();
        CHECK(client->connect("127.0.0.1", listener.localPort()), "client connect flood");
        auto server = listener.accept();
        CHECK(server != nullptr, "accept flood");

        const std::vector<std::byte> chunk(1u << 20, std::byte{'z'});  // 1MB
        // 不 pump 接收端，两次 8MB 写后内核缓冲必然打满，send 返回 EAGAIN
        for (int i = 0; i < 16; ++i) {
            client->write(std::span<const std::byte>(chunk.data(), chunk.size()));
        }
        // 若 wouldBlock 分支未触发（缓冲异常巨大），继续追加
        for (int i = 0; i < 64 && client->isConnected(); ++i) {
            client->write(std::span<const std::byte>(chunk.data(), chunk.size()));
        }
        CHECK(client->isConnected(), "EAGAIN should not mark disconnected");

        server->close();
        client->close();
        listener.close();
    }

    // 服务端 close 后继续写：首个 send 成功但引来 RST，此后
    // send 走 EPIPE 分支（标记断开）、recv 走 ECONNRESET 分支。
    // 两条连接分别触发两个分支（pump 会先置断开，互不干扰）。
    {
        std::byte b{0x01};
        auto makePair = [](TcpListener& l, std::shared_ptr<TcpConnection>& client,
                           std::shared_ptr<TcpConnection>& server) {
            if (!l.listen("127.0.0.1", 0)) return false;
            client = TcpConnection::create();
            if (!client->connect("127.0.0.1", l.localPort())) return false;
            server = l.accept();
            return server != nullptr;
        };

        // EPIPE：close 后写两次，等 RST 到达后第二次 send 报错
        {
            TcpListener listener;
            std::shared_ptr<TcpConnection> client;
            std::shared_ptr<TcpConnection> server;
            if (!makePair(listener, client, server)) {
                CHECK(false, "setup epipe pair");
            } else {
                server->close();
                CHECK(client->write(std::span<const std::byte>(&b, 1)),
                      "first write after peer close is buffered");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                client->write(std::span<const std::byte>(&b, 1));  // EPIPE
                CHECK(!client->isConnected(), "send error marks disconnected");
                client->close();
            }
            listener.close();
        }

        // ECONNRESET：close 后写一次，等 RST 到达后 pump 的 recv 报错
        {
            TcpListener listener;
            std::shared_ptr<TcpConnection> client;
            std::shared_ptr<TcpConnection> server;
            if (!makePair(listener, client, server)) {
                CHECK(false, "setup reset pair");
            } else {
                server->close();
                static_cast<void>(client->write(std::span<const std::byte>(&b, 1)));
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                client->pump();  // recv → ECONNRESET
                CHECK(!client->isConnected(), "recv error marks disconnected");
                client->close();
            }
            listener.close();
        }
        // 连接被拒：非阻塞 connect 先 in-progress，拒绝以 RST 形式到达，
        // pump 的 recv 报错走 !wouldBlock 分支（ECONNRESET/REFUSED）
        {
            auto c = TcpConnection::create();
            if (c->connect("127.0.0.1", 1)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                c->pump();
                CHECK(!c->isConnected(), "refused connect marks disconnected on pump");
            }
            c->close();
        }
    }

    // 空数据 write：已连接时返回 true 且不入缓冲；重复 listen 返回 false；
    // onReceived 回调链路。
    {
        TcpListener listener;
        std::shared_ptr<TcpConnection> client;
        std::shared_ptr<TcpConnection> server;
        bool ok = true;
        if (!listener.listen("127.0.0.1", 0)) {
            CHECK(false, "setup empty-write pair");
        } else {
            client = TcpConnection::create();
            ok = ok && client->connect("127.0.0.1", listener.localPort());
            server = listener.accept();
            ok = ok && server != nullptr;

            // 已连接 + 空数据 → 返回 connected_（true），不走 send。
            CHECK(client->write(std::span<const std::byte>{}), "empty write on connected");
            // 未连接 + 空数据 → 返回 false。
            auto dead = TcpConnection::create();
            CHECK(!dead->write(std::span<const std::byte>{}), "empty write on disconnected");

            // 重复 listen 同一 listener → false。
            CHECK(!listener.listen("127.0.0.1", 0), "second listen rejected");

            // onReceived 回调：client 发数据，server pump 后回调收到。
            int received = 0;
            std::byte lastByte{0};
            server->setOnReceived([&](std::span<const std::byte> data) {
                if (!data.empty()) {
                    lastByte = data.back();
                    ++received;
                }
            });
            const std::byte sent{0x7E};
            ok = ok && client->write(std::span<const std::byte>(&sent, 1));
            for (int i = 0; i < 50 && received == 0; ++i) {
                client->pump();
                server->pump();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            ok = ok && received == 1 && lastByte == sent;
            CHECK(ok, "onReceived callback path");
            client->close();
            server->close();
        }
        listener.close();
    }

    TcpConnection::globalShutdown();

    // connectStillPending 语义集直测：pending 窗口（SO_ERROR ∈ EINPROGRESS 系）
    // 只出现在握手跨 tick 未落定的真实网络——内核回环握手同步完成，运行时
    // 打不中，纯函数直测锚定 errno 契约：pending 系 vs 最终错误/成功。
    CHECK(theseed::runtime::detail::connectStillPending(EINPROGRESS),
          "EINPROGRESS is pending");
    CHECK(theseed::runtime::detail::connectStillPending(EINTR), "EINTR is pending");
    CHECK(theseed::runtime::detail::connectStillPending(EAGAIN), "EAGAIN is pending");
    CHECK(theseed::runtime::detail::connectStillPending(EWOULDBLOCK),
          "EWOULDBLOCK is pending");
    CHECK(!theseed::runtime::detail::connectStillPending(ECONNREFUSED),
          "ECONNREFUSED is final");
    CHECK(!theseed::runtime::detail::connectStillPending(0),
          "zero (handshake done) is final");

    // RST 断连臂 A（recv 侧）：对端以 SO_LINGER{1,0} 裸 close 触发 RST——
    // TcpConnection 的 close 走正常 FIN，无法产生 RST，借裸 socket 注入。
    // RST 到达后 recv 返回 ECONNRESET（非 EAGAIN 的 recv 真错误）→ 断连。
    {
        TcpListener listener;
        CHECK(listener.listen("127.0.0.1", 0), "listen for RST recv arm");
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in rawAddr{};
        rawAddr.sin_family = AF_INET;
        rawAddr.sin_port = htons(listener.localPort());
        rawAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(::connect(fd, reinterpret_cast<sockaddr*>(&rawAddr),
                        sizeof(rawAddr)) == 0,
              "raw client connect");
        auto server = listener.accept();
        CHECK(server != nullptr, "accept RST recv arm");
        linger rstLinger{1, 0};  // on=1 timeout=0：close 无条件 RST
        ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &rstLinger, sizeof(rstLinger));
        ::close(fd);
        std::this_thread::sleep_for(
            std::chrono::milliseconds{20});  // 回环 RST 必达
        server->pump();
        CHECK(!server->isConnected(), "RST must disconnect via recv ECONNRESET");
        server->close();
    }

    // RST 断连臂 B（send 侧）：同一注入，先 write——send 返回真错误
    // （EPIPE 系，非 EAGAIN 且非 ENOTCONN）→ 断连，连接状态不再乐观。
    {
        TcpListener listener;
        CHECK(listener.listen("127.0.0.1", 0), "listen for RST send arm");
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in rawAddr{};
        rawAddr.sin_family = AF_INET;
        rawAddr.sin_port = htons(listener.localPort());
        rawAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(::connect(fd, reinterpret_cast<sockaddr*>(&rawAddr),
                        sizeof(rawAddr)) == 0,
              "raw client connect");
        auto server = listener.accept();
        CHECK(server != nullptr, "accept RST send arm");
        linger rstLinger{1, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &rstLinger, sizeof(rstLinger));
        ::close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        const std::byte one{0x01};
        server->write(std::span<const std::byte>(&one, 1));
        CHECK(!server->isConnected(), "RST must disconnect via send error");
        server->close();
    }

    if (gFailures == 0) {
        std::cout << "TcpConnectionTest: all passed" << std::endl;
    } else {
        std::cout << "TcpConnectionTest: " << gFailures << " failure(s)" << std::endl;
    }
    return gFailures == 0 ? 0 : 1;
}
