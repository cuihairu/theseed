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
    //（返回 nullptr = 本次尝试拿不到 transport：只告警排重试；链路未接通期间
    // dbRequest 立即按 NotConnected 失败）。重连按退避重新调用本 seam 铸造新
    // transport——与 machine 腿同款（每次尝试独立铸造，测试可脚本失败序列）。
    std::function<std::shared_ptr<runtime::IRuntimeTransport>(const std::string& host,
                                                              std::uint16_t port)> dbTransportFactory;
    // db 腿运行期韧性（与通知腿同款监督模式，04 §8）：出站链路断开（DBApp
    // 重启/掉线）或接通探针应答超窗后，tick 里的监督面按指数退避重连并重发
    // db.listTypes 接通探针——复用 hub「首请求 sourceComponent 自报」注册
    //（与 machine 腿探针同机制）。选 listTypes 而非 queryAccount：其 .ok
    // 应答方法不与任何登录请求的应答匹配串重叠，迟到的探针应答不可能被
    // 在途 dbRequest 误认作登录应答。断连窗口内的 db 请求照旧走 dbRequest
    // 超时降级（发送即 NotConnected → "database unavailable"），不阻塞
    // tick、不改变登录失败语义。
    //   - 退避序列：base 起步，每次断链/失败尝试翻倍，封顶 max；
    //     链路恢复（收到 DBApp 任一应答）复位为 base。
    //   - ackTimeout：一次连接尝试后等待 DBApp 探针应答的上限，超时视为
    //     该次尝试未接通——活性以收到流量为准，不以 connect 返回为准
    //    （Linux 非阻塞 connect 恒 EINPROGRESS）。
    std::chrono::milliseconds dbReconnectBaseDelay{1000};
    std::chrono::milliseconds dbReconnectMaxDelay{30000};
    std::chrono::milliseconds dbProbeAckTimeout{5000};
    // 控制面通知腿（04 §8 踢人联动生产接线）：MachineDaemon 地址。非空时
    // init 出站连 daemon（hub connectPeer）并发一条 machine.snapshot 注册
    // 探针——daemon 侧 hub 由首条请求的 sourceComponent 自报注册
    //（attachServerTransport seam，与 DBApp 同机制），此后 daemon 的
    // machine.session.revoked 推送沿该连接入站，由 handleInvocation 分发
    // 到 handleSessionRevoked。空 = 不接线（联动通知收不到，登录面不受
    // 影响）。连接失败同样不阻断登录面（联动是 best-effort 增益）：
    // 转入退避重连的监督状态机，运行期自愈（见下方韧性三参数）。
    std::string machineHost;
    std::uint16_t machinePort = 0;
    runtime::ComponentId machineComponentId = 60;  // Machine 组件默认 id
    // 测试/嵌入注入点：与 dbTransportFactory 同款 seam（返回 nullptr =
    // 跳过通知腿）。重连按退避重新调用本 seam 铸造新 transport——测试
    // 由此可确定性模拟断链恢复（每次尝试可返回不同模式的桩）。
    std::function<std::shared_ptr<runtime::IRuntimeTransport>(const std::string& host,
                                                              std::uint16_t port)> machineTransportFactory;
    // 通知腿运行期韧性（04 §8）：出站连接断开（daemon 重启/掉线）或
    // 注册探针应答超时后，tick 里的监督面按指数退避重连并重发探针，
    // 恢复 daemon 侧的订阅注册。断连期间通知照旧 best-effort 丢弃——
    // 只告警计数，不阻断登录面。
    //   - 退避序列：base 起步，每次断链/失败尝试翻倍，封顶 max；
    //     链路恢复（收到 daemon 任一入站）复位为 base。
    //   - ackTimeout：一次连接尝试后等待 daemon 首个入站（探针应答或
    //     推送）的上限，超时视为该次尝试未接通（daemon 端口被占但不
    //     应答等半开形态）——活性以收到流量为准，不以 connect 返回为准
    //     （Linux 非阻塞 connect 恒 EINPROGRESS）。
    std::chrono::milliseconds machineReconnectBaseDelay{1000};
    std::chrono::milliseconds machineReconnectMaxDelay{30000};
    std::chrono::milliseconds machineProbeAckTimeout{5000};
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

    // 出站链路监督状态机（通知腿与 db 腿同款，04 §8 运行期韧性）。
    //   PendingAck = 已连接已发探针，等对端首个入站证实活性；
    //   Up         = 活性已证实（应答或推送任一入站皆可）；
    //   Backoff    = 断链/超时，等退避窗口到期重连重探针。
    // 活性判定走 IRuntimeTransport::isConnected（对端 EOF 由 hub 泵
    // 触发 socket 读取转为假）+ 入站证实——不以 connect 返回为准
    //（Linux 非阻塞 connect 恒 EINPROGRESS，连到死端口也算"成功"）。
    enum class LinkState { PendingAck, Up, Backoff };
    // 一次出站连接尝试：factory 或真实 TcpConnection 铸 transport →
    // connectPeer 覆盖注册 + 发 machine.snapshot 注册探针。失败（无
    // transport 或探针发不出）转入 Backoff 排期，不抛出、不阻断登录面。
    void attemptMachineLink();
    // 转入 Backoff：下次尝试排在 now+当前窗口，窗口翻倍（封顶 max）。
    void scheduleMachineRetry();
    // daemon 方向的入站证实链路活性：PendingAck → Up，退避窗口复位。
    // Up/Backoff 幂等（Backoff 下收到入站属理论外形态，不误升级——
    // 活性未经 probe/推送通道证实过，等下一次到点尝试即可）。
    void confirmMachineLinkUp();
    // 断链处置：告警 + 计数、从 hub 摘除死 peer、scheduleMachineRetry。
    // cause 进日志属性（transport-lost / probe-ack-timeout）。
    void markMachineLinkDown(const char* cause);
    // tick 里的监督环：PendingAck 查活性/应答超时，Up 查活性，
    // Backoff 到点重试。machineHost 为空（未接线）时不运行。
    void superviseMachineLink();

    // db 腿运行期韧性（与上方通知腿监督同款，04 §8）：同一套三段状态机
    // 与退避纪律，差异只在探针——发 db.listTypes（只读、必应答，且
    // .ok 应答方法不与登录请求匹配串重叠），且 db 腿是拉取式：Up 证实
    // 除探针应答外，任何 DBApp 方向的在途应答（含真实登录请求的）同样
    // 升级。断链窗口内 dbRequest 因 peer 缺席立即 NotConnected 失败，
    // 沿用既有超时降级语义。
    void attemptDbLink();       // 一次出站连接尝试 + 发接通探针
    void scheduleDbRetry();     // 转入 Backoff：窗口翻倍封顶 max
    void confirmDbLinkUp();     // DBApp 方向入站证实：PendingAck → Up，退避复位
    void markDbLinkDown(const char* cause);  // 断链处置：告警+计数+摘 peer+排重试
    void superviseDbLink();     // tick 监督环；db 腿未接线时不运行

    // 向 DBApp 发起一次请求-应答。等待上限为 config_.dbRequestTimeout；
    // 发送失败（NotConnected 等，含断链退避窗口内 peer 缺席）、超时或杂散
    // 应答耗尽等待窗口时返回 method 为空的 RuntimeInvocation
    //（调用方按 method 校验判失败）。
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

    // 通知腿监督面状态（仅 machineHost 非空时演进，见 superviseMachineLink）。
    // machineTransport_ 只用于活性查询与生命周期持有；入站分发经 hub 排空，
    // 重连成功时 connectPeer 以新 transport 覆盖 hub 内旧注册。
    LinkState machineLinkState_ = LinkState::Backoff;
    std::shared_ptr<runtime::IRuntimeTransport> machineTransport_;
    runtime::TimePoint machineAckDeadline_{};   // PendingAck 的应答超时点
    runtime::TimePoint machineRetryAt_{};       // Backoff 的下次尝试时点
    std::chrono::milliseconds machineRetryDelay_{0};  // 当前退避窗口（指数，封顶 max）

    // db 腿监督面状态（仅 dbLegEnabled_ 为真时演进，见 superviseDbLink）。
    // 与通知腿同款成员与纪律；dbLegEnabled_ 单独成旗是因为 db 腿的接线
    // 判据是 authType=="db" 且 dbHost 非空的双条件，host 一维不够。
    bool dbLegEnabled_ = false;
    LinkState dbLinkState_ = LinkState::Backoff;
    std::shared_ptr<runtime::IRuntimeTransport> dbTransport_;
    runtime::TimePoint dbAckDeadline_{};
    runtime::TimePoint dbRetryAt_{};
    std::chrono::milliseconds dbRetryDelay_{0};

    std::unique_ptr<ops::OpsInspector> opsInspector_;
    std::unique_ptr<ops::OpsServer> opsServer_;
};

}  // namespace theseed::login
