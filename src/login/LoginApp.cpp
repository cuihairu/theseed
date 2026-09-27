#include "theseed/login/LoginApp.h"
#include "theseed/login/ClientSession.h"
#include "theseed/login/LoginProtocol.h"
#include "theseed/login/SessionToken.h"
#include "theseed/control/machine/MachineDaemon.h"
#include "theseed/db/DBProtocol.h"
#include "theseed/foundation/Logger.h"
#include "theseed/foundation/Metrics.h"
#include "theseed/runtime/NetworkTransport.h"
#include "theseed/runtime/TcpConnection.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace theseed::login {

namespace {

// 从紧凑 JSON 对象里取字符串字段（machine.session.revoked 推送载荷的
// 消费侧）。转义集与 control 侧 escapeJsonString（MachineSnapshotCodec）
// 严格对齐：\\ \" \n \r \t；越集转义按畸形拒收——宁拒不猜：通知是
// best-effort，拒收有日志与计数，不会静默错配账号。键以 "key":"
// 整体定位（前引号防 "xxxkey" 类前缀误配）。
bool extractJsonStringField(const std::vector<std::byte>& payload,
                            const char* key,
                            std::string& out) {
    const std::string text(payload.begin(), payload.end());
    const std::string needle = std::string("\"") + key + "\":\"";
    const std::size_t pos = text.find(needle);
    if (pos == std::string::npos) {
        return false;  // 字段缺失
    }
    std::size_t cursor = pos + needle.size();
    out.clear();
    while (cursor < text.size()) {
        const char ch = text[cursor];
        if (ch == '"') {
            return true;  // 正常闭合
        }
        if (ch != '\\') {
            out.push_back(ch);
            ++cursor;
            continue;
        }
        if (cursor + 1 >= text.size()) {
            return false;  // 转义悬空
        }
        switch (text[cursor + 1]) {
            case '\\':
                out.push_back('\\');
                break;
            case '"':
                out.push_back('"');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            default:
                return false;  // 越集转义：畸形
        }
        cursor += 2;
    }
    return false;  // 未闭合
}

}  // namespace

LoginApp::LoginApp(LoginAppConfig config)
    : config_(std::move(config)) {
    listener_.setConnectionFactory([]() {
        return runtime::TcpConnection::create();
    });
}

LoginApp::~LoginApp() {
    stop();
}

void LoginApp::init() {
    if (!listener_.listen(config_.listenHost, config_.listenPort)) {
        return;
    }

    // db 腿（dbRequest 请求-应答）与 machine 腿（控制面通知收发）共用
    // 同一个 hub：peer 按 componentId 区分，receive 按 targetComponent
    // 过滤，互不串扰。
    const bool dbLeg = config_.authType == "db" && !config_.dbHost.empty();
    const bool machineLeg = !config_.machineHost.empty();
    if (dbLeg || machineLeg) {
        hub_ = std::make_shared<runtime::TransportHub>(config_.localComponentId);
    }

    // db 腿（04 §8 同款运行期韧性）：不再是一次性静态连接——首次尝试
    // 即走监督状态机（Backoff 排期零时点 = 立即尝试），其后的断链检测、
    // 摘 peer、退避重连、探针重发全在 superviseDbLink 里闭环（DBApp
    // 重启后无需人工重启 LoginApp）。首试失败同样不阻断 init：seam 返空
    // 等形态只告警排重试。
    if (dbLeg) {
        dbLegEnabled_ = true;
        dbRetryDelay_ = config_.dbReconnectBaseDelay;
        dbLinkState_ = LinkState::Backoff;
        dbRetryAt_ = runtime::Clock::now();  // 首次尝试立即进行
        attemptDbLink();
    }

    // 控制面通知腿（04 §8 踢人联动生产接线 + 运行期韧性）：出站连
    // MachineDaemon 并发一条 machine.snapshot 注册探针自报身份——daemon
    // 侧 hub 由首条请求的 sourceComponent 注册（attachServerTransport
    // seam），此后 machine.session.revoked 推送沿该连接入站（tick →
    // drainInvocations → handleInvocation）。首次尝试失败不阻断登录面
    //（联动是 best-effort 增益，与 db 腿的失败即弃不同）：转入监督状态
    // 机，tick 里按退避自动重连并重发探针（daemon 重启后订阅自愈，
    // 无需人工重启 LoginApp）。
    if (machineLeg) {
        machineRetryDelay_ = config_.machineReconnectBaseDelay;
        machineLinkState_ = LinkState::Backoff;
        machineRetryAt_ = runtime::Clock::now();  // 首次尝试立即进行
        attemptMachineLink();
    }

    if (config_.ops.enabled) {
        ops::ProcessInfo info{};  // LCOV_EXCL_BR_LINE 聚合内 string 成员构造/拷贝内联分支伪影（同 RealmApp）
        info.role = "LoginApp";
        info.version = "0.1.0";
        info.startTime = std::chrono::system_clock::now();  // LCOV_EXCL_BR_LINE ProcessInfo 聚合拷贝内联分支伪影
        info.componentId = config_.localComponentId;

        opsInspector_ = std::make_unique<ops::OpsInspector>(std::move(info), [this] {
            ops::RuntimeInfo rt{};
            rt.sessionCount = sessions_.size();
            if (hub_) {
                rt.transportStats = hub_->stats();
            }
            return rt;
        });

        ops::OpsServer::Config opsCfg{};  // LCOV_EXCL_BR_LINE 聚合内 string 成员构造/拷贝内联分支伪影（同 L55/L58）
        opsCfg.host = config_.ops.host;
        opsCfg.port = config_.ops.port;  // LCOV_EXCL_BR_LINE opsCfg 聚合拷贝的内联分支伪影归因行
        opsCfg.maxConnections = config_.ops.maxConnections;
        opsServer_ = std::make_unique<ops::OpsServer>(opsCfg, *opsInspector_);
        opsServer_->start();
    }
}

void LoginApp::tick() {
    const auto tickStart = std::chrono::steady_clock::now();
    if (hub_) {
        hub_->tick();
        // 排空发到本组件的入站 invocation（控制面推送与探针应答）；
        // 与 DBApp::processMessages 同款循环，放在 hub 泵之后。监督放
        // 在排空之后：本 tick 到达的应答/推送先升级活性，再判断链，
        // 不误伤刚恢复的链路。
        drainInvocations();
        superviseMachineLink();
        superviseDbLink();
    }
    acceptConnections();
    for (auto& session : sessions_) {
        session->pump();
    }
    cleanupDisconnected();

    // Phase B MVP metric: live login sessions.
    theseed::foundation::MetricsRegistry::instance()
        .gauge("login_pending_count", "active client sessions held by LoginApp")
        .set(static_cast<std::int64_t>(sessions_.size()));

    if (hub_) {
        transportStatsCollector_.collect(hub_->stats());
    }

    if (opsServer_) {
        opsServer_->tick();
    }

    // tick_duration_ms：LoginApp 不走 TickScheduler，自行观测，桶边界与之一致。
    const auto elapsed = std::chrono::steady_clock::now() - tickStart;
    theseed::foundation::MetricsRegistry::instance()
        .histogram("tick_duration_ms",
                   theseed::foundation::Histogram::Boundaries{1.0, 2.0, 5.0, 10.0, 25.0, 50.0, 100.0, 250.0, 500.0, 1000.0},
                   "tick wall-clock duration in milliseconds")
        .observe(std::chrono::duration<double, std::milli>(elapsed).count());
}

void LoginApp::stop() {
    sessions_.clear();
    machineTransport_.reset();
    machineLinkState_ = LinkState::Backoff;
    dbTransport_.reset();
    dbLinkState_ = LinkState::Backoff;
    hub_.reset();
    listener_.close();
}

const std::vector<RealmInfo>& LoginApp::realms() const {
    return config_.realms;
}

void LoginApp::handleClientMessage(ClientSession* session,
                                   ClientMessageType type,
                                   std::span<const std::byte> payload) {
    onClientMessage(session, type, payload);
}

// 入站排空（hub 面）：逐条取出发到本组件的 RuntimeInvocation 交给
// 分发面。循环终止于队列清空——推送与应答都是短消息，不会死循环。
void LoginApp::drainInvocations() {
    runtime::RuntimeInvocation inv;
    while (hub_->receive(config_.localComponentId, &inv, 1) > 0) {
        handleInvocation(inv);
    }
}

// 通知腿监督状态机（04 §8 运行期韧性）。活性以「看到入站流量」为准：
// Linux 非阻塞 connect 恒 EINPROGRESS（连到死端口也"成功"），唯一可靠
// 证据是 daemon 的应答/推送真正到达，以及 hub 泵 socket 后 transport
// 报死（对端 EOF 由 TcpConnection::pump 的 recv==0 转为 isConnected
// 假——NetworkTransport 委托 pipe 实况）。
void LoginApp::attemptMachineLink() {
    std::shared_ptr<runtime::IRuntimeTransport> transport;
    if (config_.machineTransportFactory) {
        transport = config_.machineTransportFactory(config_.machineHost,
                                                    config_.machinePort);
    } else {
        auto conn = runtime::TcpConnection::create();
        if (conn->connect(config_.machineHost, config_.machinePort)) {
            transport = std::make_shared<runtime::NetworkTransport>(conn);
        } else {  // LCOV_EXCL_BR_LINE Linux 非阻塞 connect 恒 EINPROGRESS，失败臂不可达（与 db 腿同理由）
            // LCOV_EXCL_START inet_pton 无 DNS：坏主机名解析为 0.0.0.0 同样 EINPROGRESS，本臂不可达
            foundation::logWarn("login.machine.link.refused", {});
            // LCOV_EXCL_STOP
        }
    }
    if (!transport) {
        // 拿不到 transport（注入 seam 返空）：只告警排重试，登录面无感。
        foundation::logWarn("login.machine.link.no-transport", {});
        scheduleMachineRetry();
        return;
    }
    // connectPeer 覆盖旧注册：重连路径以新 transport 顶替死腿，daemon
    // 侧对新连接的注册经探针自报重新建立（attachServerTransport seam）。
    hub_->connectPeer(config_.machineComponentId, transport);
    machineTransport_ = std::move(transport);
    // 注册探针 = §8 只读探活语义的 machine.snapshot：既向 daemon 自报
    // 身份（推送通道的注册面），也验证策略接线（应答 ok / error 经
    // handleInvocation 记录联动状态）。
    runtime::RuntimeInvocation probe;
    probe.sourceComponent = config_.localComponentId;
    probe.targetComponent = config_.machineComponentId;
    probe.method = control::machine::MachineMethod::kSnapshot;
    if (hub_->send(std::move(probe)) != runtime::SendResult::Accepted) {
        // 探针都发不出（对端 transport 报 NotConnected）：本次尝试未
        // 接通，摘除死 peer 退避重试——不留会持续丢推送的僵尸注册。
        foundation::logWarn("login.machine.probe.unsent", {});
        hub_->disconnectPeer(config_.machineComponentId);
        machineTransport_.reset();
        scheduleMachineRetry();
        return;
    }
    hub_->flush();
    machineLinkState_ = LinkState::PendingAck;
    machineAckDeadline_ = runtime::Clock::now() + config_.machineProbeAckTimeout;
}

void LoginApp::scheduleMachineRetry() {
    machineLinkState_ = LinkState::Backoff;
    machineRetryAt_ = runtime::Clock::now() + machineRetryDelay_;
    const auto doubled = machineRetryDelay_ * 2;
    machineRetryDelay_ = std::min(doubled, config_.machineReconnectMaxDelay);
}

void LoginApp::confirmMachineLinkUp() {
    if (machineLinkState_ != LinkState::PendingAck) {
        return;  // Up 幂等；Backoff 下入站属理论外形态，不误升级
    }
    machineLinkState_ = LinkState::Up;
    machineRetryDelay_ = config_.machineReconnectBaseDelay;  // 链路恢复，退避复位
    foundation::logInfo("login.machine.link.up", {});
    theseed::foundation::MetricsRegistry::instance()
        .counter("login_machine_link_up_count",
                 "machine notify link establishments confirmed by inbound from daemon")
        .increment();
}

void LoginApp::markMachineLinkDown(const char* cause) {
    const std::string causeText = cause;
    const foundation::LogAttribute causeAttr = {"cause", causeText};
    foundation::logWarn("login.machine.link.down", {causeAttr});
    theseed::foundation::MetricsRegistry::instance()
        .counter("login_machine_link_down_count",
                 "machine notify link losses detected by LoginApp supervision")
        .increment();
    hub_->disconnectPeer(config_.machineComponentId);
    machineTransport_.reset();
    scheduleMachineRetry();
}

void LoginApp::superviseMachineLink() {
    if (config_.machineHost.empty() || !hub_) return;  // 未接线/已 stop：无监督
    const auto now = runtime::Clock::now();
    switch (machineLinkState_) {
        case LinkState::PendingAck: {
            const bool alive = machineTransport_ && machineTransport_->isConnected();
            if (alive && now < machineAckDeadline_) break;  // 应答仍宽限
            markMachineLinkDown(alive ? "probe-ack-timeout" : "transport-lost");
            break;
        }
        case LinkState::Up:
            if (machineTransport_ && machineTransport_->isConnected()) break;
            markMachineLinkDown("transport-lost");
            break;
        case LinkState::Backoff:
            if (now < machineRetryAt_) break;
            attemptMachineLink();  // 成败皆迁移状态：接通 PendingAck，未接通重排 Backoff
            break;
    }
}

// db 腿监督（04 §8 同款韧性，登录数据面的传输层自愈）。与通知腿的差异：
//   - 探针选 db.listTypes：只读、DBApp 恒应答，且应答方法
//     "db.listTypes.ok" 不与任何登录请求（queryAccount/createAccount）
//     的应答匹配串重叠——迟到的探针应答绝不会被在途 dbRequest 误认为
//     登录应答（若用 queryAccount 探针则二者同串，存在错配窗口）。
//   - db 腿是拉取式：Up 证实除排空面的探针应答外，dbRequest 等待循环里
//     收到的任何 DBApp 应答（含真实登录的）也升级活性——链路真正可用
//     的事实点本就在应答到达处。
// 断链窗口内 peer 已从 hub 摘除：dbRequest 的 send 立即 NotConnected，
// 登录按既有 "database unavailable" 语义失败，tick 不额外等待。
void LoginApp::attemptDbLink() {
    std::shared_ptr<runtime::IRuntimeTransport> transport;
    if (config_.dbTransportFactory) {
        transport = config_.dbTransportFactory(config_.dbHost, config_.dbPort);
    } else {
        auto conn = runtime::TcpConnection::create();
        if (conn->connect(config_.dbHost, config_.dbPort)) {
            transport = std::make_shared<runtime::NetworkTransport>(conn);
        } else {  // LCOV_EXCL_BR_LINE Linux 非阻塞 connect 恒 EINPROGRESS，失败臂不可达（与 machine 腿同理由）
            // LCOV_EXCL_START inet_pton 无 DNS：坏主机名解析为 0.0.0.0 同样 EINPROGRESS，本臂不可达
            foundation::logWarn("login.db.link.refused", {});
            // LCOV_EXCL_STOP
        }
    }
    if (!transport) {
        // 拿不到 transport（注入 seam 返空）：只告警排重试，登录面无感。
        foundation::logWarn("login.db.link.no-transport", {});
        scheduleDbRetry();
        return;
    }
    // connectPeer 覆盖旧注册：重连路径以新 transport 顶替死腿；DBApp 侧
    // 对新连接的注册由其 hub 在探针首请求到达时自报完成（与 machine 腿
    // attachServerTransport 同机制）。
    hub_->connectPeer(config_.dbComponentId, transport);
    dbTransport_ = std::move(transport);
    runtime::RuntimeInvocation probe;
    probe.sourceComponent = config_.localComponentId;
    probe.targetComponent = config_.dbComponentId;
    probe.method = db::DBMethod::kListTypes;
    if (hub_->send(std::move(probe)) != runtime::SendResult::Accepted) {
        // 探针都发不出（对端 transport 报 NotConnected）：本次尝试未
        // 接通，摘除死 peer 退避重试——不留挂着空注册的链路。
        foundation::logWarn("login.db.probe.unsent", {});
        hub_->disconnectPeer(config_.dbComponentId);
        dbTransport_.reset();
        scheduleDbRetry();
        return;
    }
    hub_->flush();
    dbLinkState_ = LinkState::PendingAck;
    dbAckDeadline_ = runtime::Clock::now() + config_.dbProbeAckTimeout;
}

void LoginApp::scheduleDbRetry() {
    dbLinkState_ = LinkState::Backoff;
    dbRetryAt_ = runtime::Clock::now() + dbRetryDelay_;
    const auto doubled = dbRetryDelay_ * 2;
    dbRetryDelay_ = std::min(doubled, config_.dbReconnectMaxDelay);
}

void LoginApp::confirmDbLinkUp() {
    if (dbLinkState_ != LinkState::PendingAck) {
        return;  // Up 幂等；Backoff 下入站属理论外形态，不误升级
    }
    dbLinkState_ = LinkState::Up;
    dbRetryDelay_ = config_.dbReconnectBaseDelay;  // 链路恢复，退避复位
    foundation::logInfo("login.db.link.up", {});
    theseed::foundation::MetricsRegistry::instance()
        .counter("login_db_link_up_count",
                 "db link establishments confirmed by inbound from DBApp")
        .increment();
}

void LoginApp::markDbLinkDown(const char* cause) {
    const std::string causeText = cause;
    const foundation::LogAttribute causeAttr = {"cause", causeText};
    foundation::logWarn("login.db.link.down", {causeAttr});
    theseed::foundation::MetricsRegistry::instance()
        .counter("login_db_link_down_count",
                 "db link losses detected by LoginApp supervision")
        .increment();
    hub_->disconnectPeer(config_.dbComponentId);
    dbTransport_.reset();
    scheduleDbRetry();
}

void LoginApp::superviseDbLink() {
    if (!dbLegEnabled_ || !hub_) return;  // 未接线/已 stop：无监督
    const auto now = runtime::Clock::now();
    switch (dbLinkState_) {
        case LinkState::PendingAck: {
            const bool alive = dbTransport_ && dbTransport_->isConnected();
            if (alive && now < dbAckDeadline_) break;  // 应答仍宽限
            markDbLinkDown(alive ? "probe-ack-timeout" : "transport-lost");
            break;
        }
        case LinkState::Up:
            if (dbTransport_ && dbTransport_->isConnected()) break;
            markDbLinkDown("transport-lost");
            break;
        case LinkState::Backoff:
            if (now < dbRetryAt_) break;
            attemptDbLink();  // 成败皆迁移状态：接通 PendingAck，未接通重排 Backoff
            break;
    }
}

void LoginApp::handleInvocation(const runtime::RuntimeInvocation& inv) {
    // daemon 方向任何入站（探针应答或推送）都证实出站链路活性：监督
    // 状态机据此把 PendingAck 升级为 Up——订阅通道真正可用的事实点。
    if (inv.sourceComponent == config_.machineComponentId) {
        confirmMachineLinkUp();
    }
    // DBApp 方向入站同理（排空面看到的通常是接通探针的应答；登录应答
    // 一般在 dbRequest 等待循环里就地证实，见该处 confirmDbLinkUp）。
    // listTypes.ok 是探针应答：记接通就绪后消费掉，不落入未知 method
    // 计数（探针是自家发起的，应答不是异常）。
    if (inv.sourceComponent == config_.dbComponentId) {
        confirmDbLinkUp();
        if (inv.method == db::DBMethod::kListTypesOk) {
            foundation::logInfo("login.db.link.ready", {});
            return;
        }
    }
    if (inv.method == control::machine::MachineMethod::kSessionRevoked) {
        std::string account, realm;
        if (!extractJsonStringField(inv.payload, "account", account) ||
            !extractJsonStringField(inv.payload, "realm", realm)) {
            foundation::logWarn("login.session.revoked.malformed", {});
            theseed::foundation::MetricsRegistry::instance()
                .counter("login_session_notify_malformed_count",
                         "session-revoked pushes dropped because the payload did not parse")
                .increment();
            return;
        }
        const std::size_t closed = handleSessionRevoked(account, realm);
        const foundation::LogAttribute accountAttr = {"account", account};
        const foundation::LogAttribute realmAttr = {"realm", realm};
        const foundation::LogAttribute closedAttr = {
            "closed", static_cast<std::int64_t>(closed)};
        foundation::logInfo("login.session.revoked",
                            {accountAttr, realmAttr, closedAttr});
        return;
    }
    // 注册探针应答：snapshot.ok = 联动腿接通且被信任（运维可见）；
    // machine.error = 探针被策略拒绝——两者都经 confirmMachineLinkUp
    // 证实链路活性（注册在传输层，与策略门独立：推送通道照常可用），
    // 拒绝本身不重试，运维据 daemon 侧审计排查接线。
    if (inv.method == control::machine::MachineMethod::kSnapshotOk) {
        foundation::logInfo("login.machine.link.ready", {});
        return;
    }
    if (inv.method == control::machine::MachineMethod::kError) {
        const std::string reason(inv.payload.begin(), inv.payload.end());
        const foundation::LogAttribute reasonAttr = {"reason", reason};
        foundation::logWarn("login.machine.probe.rejected", {reasonAttr});
        return;
    }
    // 未知 method：不静默丢包，记日志与计数（与 daemon 未知方法臂同族
    // 口径；无应答语义，见头注释）。
    const foundation::LogAttribute methodAttr = {"method", inv.method};
    foundation::logWarn("login.invocation.unknown", {methodAttr});
    theseed::foundation::MetricsRegistry::instance()
        .counter("login_unknown_invocation_count",
                 "inbound hub invocations with no handler; logged, not dropped silently")
        .increment();
}

runtime::RuntimeInvocation LoginApp::dbRequest(const std::string& method,
                                                std::span<const std::byte> payload) {
    runtime::RuntimeInvocation inv;
    inv.sourceComponent = config_.localComponentId;
    inv.targetComponent = config_.dbComponentId;
    inv.entityId = 0;
    inv.method = method;
    inv.payload = std::vector<std::byte>(payload.begin(), payload.end());

    if (hub_->send(std::move(inv)) != runtime::SendResult::Accepted) {
        return {};
    }
    hub_->flush();

    // 等待应答，上限 dbRequestTimeout：DBApp 失联时退化为失败返回而不是忙等挂死。
    // 单 DBApp 拓扑下杂散消息只会是过期应答，丢弃后继续等本次的。
    // 同时排空 machine 腿的推送与探针应答——防止阻塞期间堆积。
    const auto deadline = runtime::Clock::now() + config_.dbRequestTimeout;
    const auto expect = std::string(method) + ".ok";
    runtime::RuntimeInvocation resp;
    while (runtime::Clock::now() < deadline) {
        hub_->tick();
        if (hub_->receive(config_.localComponentId, &resp, 1) > 0) {
            // db 腿拉取式的活性事实点在应答到达处：本循环直接消费的应答
            // 不经排空面，这里补证实（PendingAck → Up，幂等；Up 下无操作）。
            if (resp.sourceComponent == config_.dbComponentId) {
                confirmDbLinkUp();
            }
            if (resp.method == expect) return resp;
            // 非本请求的入站（machine 腿推送、过期 db 应答）：逐条分发
            handleInvocation(resp);
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return {};
}  // LCOV_EXCL_BR_LINE 函数尾汇合伪边归因本行：各 return 臂均已由 dbRequest 场景组覆盖

void LoginApp::acceptConnections() {
    while (auto conn = listener_.accept()) {
        conn->setOnReceived(nullptr);
        auto session = std::make_unique<ClientSession>(conn);

        auto* rawSession = session.get();
        session->setMessageCallback(
            [this, rawSession](ClientMessageType type, std::span<const std::byte> payload) {
                onClientMessage(rawSession, type, payload);
            });

        sessions_.push_back(std::move(session));
    }
}

void LoginApp::onClientMessage(ClientSession* session,
                               ClientMessageType type,
                               std::span<const std::byte> payload) {
    switch (type) {
        case ClientMessageType::Login: {
            std::string account, password;
            if (LoginProtocol::decodeLogin(payload, account, password)) {
                handleLogin(session, account, password);
            }
            break;
        }
        case ClientMessageType::QueryRealms:
            handleQueryRealms(session);
            break;
        case ClientMessageType::SelectRealm: {
            std::string realmId;
            if (LoginProtocol::decodeSelectRealm(payload, realmId)) {
                handleSelectRealm(session, realmId);
            }
            break;
        }
        default:
            break;
    }
}

void LoginApp::handleLogin(ClientSession* session,
                           const std::string& account,
                           const std::string& password) {
    LoginResponse resp;

    // 限流：按 account 维度节流登录尝试。放在鉴权之前，避免无谓的 DB 往返。
    if (config_.rateLimiter && !account.empty() &&
        !config_.rateLimiter->tryConsume("login:" + account, config_.rateLimitConfig)) {  // LCOV_EXCL_BR_LINE 本行残余冷块为 string 临时量构造的 gcc 副本边（两臂恒 0）：tryConsume 真假两臂已由限流/通过场景覆盖
        resp.success = false;
        resp.error = "rate limited";
        theseed::foundation::MetricsRegistry::instance()
            .counter("login_rate_limited_count",
                     "login attempts rejected by rate limiter")
            .increment();
        auto data = LoginProtocol::encodeLoginResponse(resp);
        session->send(std::span<const std::byte>(data.data(), data.size()));
        return;
    }

    // 记录成功登录后写入会话存储的辅助 lambda。
    // realmId 在 login 阶段为空（SelectRealm 时才确定），这里先存基础会话，
    // 由 handleSelectRealm 在确定 realm 后补写。
    auto persistSession = [&](const std::string& token, std::int64_t userId) {
        if (config_.sessionStore && !token.empty()) {  // LCOV_EXCL_BR_LINE !token.empty() 防御臂不可达：persistSession 四个调用点的 token 均由 SessionToken::issue 生成恒非空
            foundation::StoredSession s;
            s.accountId = account;
            s.userId = userId;
            config_.sessionStore->save(token, s, config_.sessionTtl);
        }
    };

    if (config_.authType == "null") {
        resp.success = true;
        resp.token = SessionToken::issue(account, "");
        resp.realms = config_.realms;
        persistSession(resp.token, 0);
    } else if (config_.authType == "db" && hub_) {
        // Query DBApp for account
        auto req = db::DBProtocol::encodeQueryAccountRequest(account);
        auto respPayload = dbRequest(db::DBMethod::kQueryAccount,
                                      std::span<const std::byte>(req.data(), req.size()));

        if (respPayload.method != db::DBMethod::kQueryAccountOk) {
            // DBApp 失联/超时：不能确定账号是否存在，直接失败（不进 auto-register）
            resp.success = false;
            resp.error = "database unavailable";
            theseed::foundation::MetricsRegistry::instance()
                .counter("login_db_unavailable_count",
                         "login attempts aborted because DBApp did not answer in time")
                .increment();
        } else {
            bool found = false;
            core::EntityId accountId = 0;
            std::string storedPassword;
            db::DBProtocol::decodeQueryAccountResponse(
                std::span<const std::byte>(respPayload.payload.data(), respPayload.payload.size()),
                found, accountId, storedPassword);

            if (found && storedPassword == password) {
                resp.success = true;
                resp.token = SessionToken::issue(account, "");
                resp.realms = config_.realms;
                persistSession(resp.token, static_cast<std::int64_t>(accountId));
            } else if (!found) {
                // Auto-register: create new account
                auto createReq = db::DBProtocol::encodeCreateAccountRequest(account, password);
                auto createRespPayload = dbRequest(db::DBMethod::kCreateAccount,
                                                    std::span<const std::byte>(createReq.data(), createReq.size()));
                bool created = false;
                if (createRespPayload.method == db::DBMethod::kCreateAccountOk) {
                    db::DBProtocol::decodeCreateAccountResponse(
                        std::span<const std::byte>(createRespPayload.payload.data(), createRespPayload.payload.size()),
                        created, accountId);
                }

                if (created) {
                    resp.success = true;
                    resp.token = SessionToken::issue(account, "");
                    resp.realms = config_.realms;
                    persistSession(resp.token, static_cast<std::int64_t>(accountId));
                } else {
                    resp.success = false;
                    resp.error = "account creation failed";
                    theseed::foundation::MetricsRegistry::instance()
                        .counter("challenge_failure_count",
                                 "any login authentication failure (auth failures, account creation failures, fallback rejection)")
                        .increment();
                }
            } else {
                resp.success = false;
                resp.error = "invalid credentials";
                theseed::foundation::MetricsRegistry::instance()
                    .counter("challenge_failure_count",
                             "any login authentication failure (auth failures, account creation failures, fallback rejection)")
                    .increment();
            }
        }
    } else {
        // Fallback "password" mode: check non-empty
        if (!account.empty() && !password.empty()) {
            resp.success = true;
            resp.token = SessionToken::issue(account, "");
            resp.realms = config_.realms;
            persistSession(resp.token, 0);
        } else {
            resp.success = false;
            resp.error = "invalid credentials";
            theseed::foundation::MetricsRegistry::instance()
                .counter("challenge_failure_count",
                         "any login authentication failure (auth failures, account creation failures, fallback rejection)")
                .increment();
        }
    }

    if (resp.success) {
        // 联动绑定：登录成功即登记（领域为空——SelectRealm 成功后补，
        // 补时同步写回存储行，见 handleSelectRealm）。
        LoginBinding binding;
        binding.account = account;
        binding.token = resp.token;  // 仅供本进程补写存储行（见 handleSelectRealm）
        bindings_[session] = std::move(binding);
    }
    auto data = LoginProtocol::encodeLoginResponse(resp);
    session->send(std::span<const std::byte>(data.data(), data.size()));
}

void LoginApp::handleQueryRealms(ClientSession* session) {
    auto data = LoginProtocol::encodeRealmList(config_.realms);
    session->send(std::span<const std::byte>(data.data(), data.size()));
}

void LoginApp::handleSelectRealm(ClientSession* session, const std::string& realmId) {
    SelectRealmResponse resp;

    const RealmInfo* found = nullptr;
    for (auto& r : config_.realms) {
        if (r.realmId == realmId) {
            found = &r;
            break;
        }
    }

    if (found) {
        resp.success = true;
        resp.host = found->host;
        resp.port = found->port;
        resp.token = SessionToken::issue("", found->realmId);
        // 联动绑定补领域：只在已登录连接上更新（未登录的选领域不建绑定
        //——绑定以登录为准，防 operator[] 给陌生键开洞）。
        const auto bound = bindings_.find(session);
        if (bound != bindings_.end()) {
            bound->second.realm = realmId;
            // 存储行同步领域（联动配对的关键）：踢人通知的 realm 取自
            // 会话行，登录时只存基础会话（领域空），不补则通知恒带空
            // 领域、与本连接的绑定对不上，联动会漏关"已选领域"的连接。
            // 先 load 再改写：已过期的行不因选领域复活。TTL 仍按
            // config_.sessionTtl 重写（进入领域即该领域会话的起点），
            // 运维续期策略（extend-sessions）不受影响。
            if (config_.sessionStore) {
                const auto stored = config_.sessionStore->load(bound->second.token);
                if (stored) {
                    foundation::StoredSession patched = *stored;
                    patched.realmId = realmId;
                    config_.sessionStore->save(bound->second.token, patched,
                                               config_.sessionTtl);
                }
            }
        }
    } else {
        resp.success = false;
        resp.error = "realm not found";
    }

    auto data = LoginProtocol::encodeSelectRealmResponse(resp);
    session->send(std::span<const std::byte>(data.data(), data.size()));
}

void LoginApp::cleanupDisconnected() {
    sessions_.erase(
        std::remove_if(sessions_.begin(), sessions_.end(),
                       [this](const std::unique_ptr<ClientSession>& s) {
                           if (!s->isConnected()) {
                               // 绑定表与连接同寿：摘除连接即摘除绑定，
                               // 不留悬垂键（后续 handleSessionRevoked
                               // 不会再碰它）。
                               bindings_.erase(s.get());
                               return true;
                           }
                           return false;
                       }),
        sessions_.end());
}

std::size_t LoginApp::handleSessionRevoked(const std::string& accountId,
                                            const std::string& realmId) {
    std::size_t closed = 0;
    for (auto& [session, binding] : bindings_) {
        // 只关活跃连接（已断开的留待 cleanupDisconnected 收敛）；领域
        // 精确匹配——空领域 = 登录后未选领域，与存储行同形。
        if (session->isConnected() && binding.account == accountId &&
            binding.realm == realmId) {
            session->close();
            ++closed;
        }
    }
    if (closed > 0) {
        theseed::foundation::MetricsRegistry::instance()
            .counter("login_session_revoked_count",
                     "live logins closed by session revocation linkage")
            .increment(static_cast<std::uint64_t>(closed));
    }
    return closed;
}

}  // namespace theseed::login
