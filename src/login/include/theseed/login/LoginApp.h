#pragma once

#include "theseed/foundation/RateLimiter.h"
#include "theseed/foundation/RedisProvider.h"
#include "theseed/foundation/SessionStore.h"
#include "theseed/login/LoginProtocol.h"
#include "theseed/login/LoginTypes.h"
#include "theseed/ops/OpsInspector.h"
#include "theseed/ops/OpsServer.h"
#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/TcpListener.h"
#include "theseed/runtime/TransportHub.h"
#include "theseed/runtime/TransportStatsCollector.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace theseed::login {

class ClientSession;

struct LoginAppOpsConfig {
    bool enabled = false;
    std::string host = "127.0.0.1";
    std::uint16_t port = 20099 + 100;  // avoid colliding with listen port
    std::size_t maxConnections = 8;
};

struct LoginAppConfig {
    std::string listenHost = "0.0.0.0";
    std::uint16_t listenPort = 20099;
    std::string authType = "password";  // "password" | "null" | "db"
    std::vector<RealmInfo> realms;
    std::string dbHost;
    std::uint16_t dbPort = 20003;
    runtime::ComponentId dbComponentId = 10;
    runtime::ComponentId localComponentId = 20;
    // dbRequest 等待 DBApp 应答的上限。超时/发送失败按 "database unavailable" 处理，
    // 避免 DBApp 失联时忙等挂死。
    std::chrono::milliseconds dbRequestTimeout{5000};
    // 测试/嵌入注入点：非空时跳过真实 TcpConnection，直接使用返回的 transport
    //（返回 nullptr 时该 peer 缺席，dbRequest 立即按 NotConnected 失败）。
    std::function<std::shared_ptr<runtime::IRuntimeTransport>(const std::string& host,
                                                              std::uint16_t port)> dbTransportFactory;
    LoginAppOpsConfig ops;

    // Redis 会话/限流集成（Phase B）。三者共享同一个 IRedisProvider。
    // 全部留空（nullptr）时退化为旧行为：token 只发给客户端，不做持久化与限流。
    std::shared_ptr<foundation::IRedisProvider> redis;
    std::shared_ptr<foundation::SessionStore> sessionStore;
    std::shared_ptr<foundation::RateLimiter> rateLimiter;
    foundation::RateLimiter::Config rateLimitConfig;
    // 会话 TTL。默认 1 小时。
    foundation::RedisDuration sessionTtl = std::chrono::seconds(3600);
};

class LoginApp {
public:
    explicit LoginApp(LoginAppConfig config);
    ~LoginApp();

    LoginApp(const LoginApp&) = delete;
    LoginApp& operator=(const LoginApp&) = delete;

    void init();
    void tick();
    void stop();

    const std::vector<RealmInfo>& realms() const;

    // 测试入口：在不需要 TCP 监听的情况下驱动消息分发（含限流与会话存储）。
    // 生产路径由 acceptConnections 自动调用 onClientMessage，不需要此方法。
    void handleClientMessage(ClientSession* session,
                             ClientMessageType type,
                             std::span<const std::byte> payload);

private:
    void acceptConnections();
    void onClientMessage(ClientSession* session,
                         ClientMessageType type,
                         std::span<const std::byte> payload);
    void handleLogin(ClientSession* session,
                     const std::string& account,
                     const std::string& password);
    void handleQueryRealms(ClientSession* session);
    void handleSelectRealm(ClientSession* session, const std::string& realmId);
    void cleanupDisconnected();

    // 向 DBApp 发起一次请求-应答。等待上限为 config_.dbRequestTimeout；
    // 发送失败（NotConnected 等）、超时或杂散应答耗尽等待窗口时返回
    // method 为空的 RuntimeInvocation（调用方按 method 校验判失败）。
    runtime::RuntimeInvocation dbRequest(const std::string& method,
                                          std::span<const std::byte> payload);

    LoginAppConfig config_;
    runtime::TcpListener listener_;
    std::shared_ptr<runtime::TransportHub> hub_;
    std::vector<std::unique_ptr<ClientSession>> sessions_;
    runtime::TransportStatsCollector transportStatsCollector_;

    std::unique_ptr<ops::OpsInspector> opsInspector_;
    std::unique_ptr<ops::OpsServer> opsServer_;
};

}  // namespace theseed::login
