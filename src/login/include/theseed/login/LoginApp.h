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
#include <unordered_map>
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
    // 控制面通知腿（04 §8 踢人联动生产接线）：MachineDaemon 地址。非空时
    // init 出站连 daemon（hub connectPeer）并发一条 machine.snapshot 注册
    // 探针——daemon 侧 hub 由首条请求的 sourceComponent 自报注册
    //（attachServerTransport seam，与 DBApp 同机制），此后 daemon 的
    // machine.session.revoked 推送沿该连接入站，由 handleInvocation 分发
    // 到 handleSessionRevoked。空 = 不接线（联动通知收不到，登录面不受
    // 影响）。连接失败同样只跳过不阻断（联动是 best-effort 增益）。
    std::string machineHost;
    std::uint16_t machinePort = 0;
    runtime::ComponentId machineComponentId = 60;  // Machine 组件默认 id
    // 测试/嵌入注入点：与 dbTransportFactory 同款 seam（返回 nullptr =
    // 跳过通知腿）。
    std::function<std::shared_ptr<runtime::IRuntimeTransport>(const std::string& host,
                                                              std::uint16_t port)> machineTransportFactory;
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

    // 入站 RuntimeInvocation 分发面（04 §8 踢人联动生产接线）：machine.
    // session.revoked → 解载荷 → handleSessionRevoked；注册探针的应答
    //（snapshot.ok / error）记联动状态；其余 method 记日志与计数后
    // 丢弃——不回包：本面是单向推送消费面，回 error 会与 daemon 的
    // 未知方法臂互弹成环。公开为测试入口（与 handleClientMessage 同款），
    // 生产路径由 tick 的排空循环（drainInvocations）调用。
    void handleInvocation(const runtime::RuntimeInvocation& inv);

    // 踢人联动处理面（§8 Phase 2）：按账号+领域关闭匹配的活跃登录连接
    //（MachineDaemon 吊销会话后推送 machine.session.revoked，载荷即
    // account/realm——本进程据本地绑定表定位连接）。返回关闭数。
    // 生产通路：hub 入站分发（handleInvocation）解析推送载荷后调本方法；
    // 本方法保持纯处理面（不解析协议），便于桩面直测。
    std::size_t handleSessionRevoked(const std::string& accountId,
                                     const std::string& realmId);

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
    // 排空发到本组件的入站 invocation 并逐条分发（与 DBApp::
    // processMessages 同款）；由 tick 在 hub 泵后调用。
    void drainInvocations();

    // 向 DBApp 发起一次请求-应答。等待上限为 config_.dbRequestTimeout；
    // 发送失败（NotConnected 等）、超时或杂散应答耗尽等待窗口时返回
    // method 为空的 RuntimeInvocation（调用方按 method 校验判失败）。
    runtime::RuntimeInvocation dbRequest(const std::string& method,
                                          std::span<const std::byte> payload);

    LoginAppConfig config_;
    runtime::TcpListener listener_;
    std::shared_ptr<runtime::TransportHub> hub_;
    std::vector<std::unique_ptr<ClientSession>> sessions_;
    // 已认证连接的本地绑定（账号/领域）：踢人联动的定位面。键为
    // sessions_ 内连接的指针——cleanupDisconnected 与其同步摘除，存活
    // 期与 sessions_ 元素一致（单线程假设与 ClientSession 同）。键非
    // const：命中即 close()（通知联动只关连接，不碰会话其余状态）。
    struct LoginBinding final {
        std::string account;
        std::string realm;  // 登录时为空，SelectRealm 成功后补
        // 令牌只为本进程内补写存储行而留（选领域时把 realm 落进会话行，
        // 见 handleSelectRealm）。绝不进日志、不回显、不出进程——与
        // SessionStore/通知面同纪律。
        std::string token;
    };
    std::unordered_map<ClientSession*, LoginBinding> bindings_;
    runtime::TransportStatsCollector transportStatsCollector_;

    std::unique_ptr<ops::OpsInspector> opsInspector_;
    std::unique_ptr<ops::OpsServer> opsServer_;
};

}  // namespace theseed::login
