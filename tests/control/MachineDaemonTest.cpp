// MachineDaemon 端到端测试：真实 TCP 回环驱动 LocalHostProbe +
// LocalProcessSupervisor + MachineAgent 的完整链路——snapshot JSON 往返、
// execute start/stop 受管子进程闭环、协议错误（畸形 execute 载荷/未知方法）、
// 幂等 start 与端口冲突。
#include "theseed/control/machine/HostProbe.h"
#include "theseed/control/machine/MachineAgent.h"
#include "theseed/control/machine/MachineDaemon.h"
#include "theseed/control/machine/NodeReport.h"
#include "theseed/control/machine/ProcessSupervisor.h"
#include "theseed/control/ops/OpsControlCenter.h"
#include "theseed/foundation/Logger.h"
#include "theseed/ops/OpsServer.h"
#include "theseed/foundation/Metrics.h"
#include "theseed/foundation/Tracing.h"
#include "theseed/runtime/NetworkTransport.h"
#include "theseed/runtime/TcpConnection.h"
#include "theseed/runtime/TickProfiler.h"
#include "theseed/runtime/TickScheduler.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <variant>
#include <vector>

using theseed::control::machine::AccessRole;
using theseed::control::machine::LocalHostProbe;
using theseed::control::machine::LocalProcessSupervisor;
using theseed::control::machine::MachineAgent;
using theseed::control::machine::MachineDaemon;
using theseed::control::machine::NodeAuditEntry;
using theseed::control::machine::NodeReport;
using theseed::control::machine::ProfileQuery;
using theseed::control::ops::OpsControlCenter;
namespace foundation = theseed::foundation;
// 命名空间不能 using-declare，用别名
namespace MachineMethod = theseed::control::machine::MachineMethod;
using theseed::runtime::ComponentId;
using theseed::runtime::NetworkTransport;
using theseed::runtime::RuntimeInvocation;
using theseed::runtime::SendResult;
using theseed::runtime::TcpConnection;
using theseed::runtime::TickProfiler;
using theseed::runtime::TickScheduler;

#define TEST(name)                            \
    do {                                      \
        std::cout << "  " << name << "... ";  \
    } while (0)
#define PASS() std::cout << "OK" << std::endl
#define FAIL(msg)                                   \
    do {                                            \
        std::cout << "FAILED: " << msg << std::endl; \
        return 1;                                   \
    } while (0)

namespace {

constexpr ComponentId kMachineComponent = 60;
constexpr ComponentId kClientComponent = 1;

// 裸协议客户端：直连 MachineDaemon 监听端口收发 RuntimeInvocation。
struct RawClient {
    std::shared_ptr<TcpConnection> conn;
    std::shared_ptr<NetworkTransport> transport;
    int pumpCount = 0;

    bool connect(std::uint16_t port) {
        conn = TcpConnection::create();
        if (!conn->connect("127.0.0.1", port)) return false;
        transport = std::make_shared<NetworkTransport>(conn);
        return true;
    }

    void settle(const std::function<void()>& daemonTick) {
        for (int i = 0; i < 40; ++i) {
            daemonTick();
            transport->tick();
            ::usleep(2000);
        }
        pumpCount = 0;
    }

    bool request(const std::string& method, std::vector<std::byte> payload,
                 const std::function<void()>& daemonTick, RuntimeInvocation& out) {
        // 来源身份可参数化：策略测试用第二个组件身份验证拒绝路径
        // （服务端 hub 由首条请求的 sourceComponent 自报注册）。
        const auto component = this->component;
        RuntimeInvocation inv;
        inv.sourceComponent = component;
        inv.targetComponent = kMachineComponent;
        inv.method = method;
        inv.payload = std::move(payload);
        if (transport->send(std::move(inv)) != SendResult::Accepted) return false;
        transport->flush();
        for (int i = 0; i < 4000; ++i) {
            if (++pumpCount > 20000) {
                std::cout << "FAILED: pump guard tripped" << std::endl;
                std::exit(1);
            }
            daemonTick();
            transport->tick();
            ::usleep(500);
            if (transport->receive(component, &out, 1) > 0) return true;
        }
        return false;
    }

    ComponentId component = kClientComponent;
};

std::string payloadToString(const RuntimeInvocation& inv) {
    return std::string(reinterpret_cast<const char*>(inv.payload.data()),
                       inv.payload.size());
}

std::vector<std::byte> payloadOf(const std::string& text) {
    std::vector<std::byte> bytes(text.size());
    if (!text.empty()) {
        std::memcpy(bytes.data(), text.data(), text.size());
    }
    return bytes;
}

// execute 请求载荷：command '\0' args（不能用字符串字面量拼——string_view
// 构造在首个 NUL 截断）。
std::vector<std::byte> executePayload(const std::string& command, const std::string& args) {
    return payloadOf(command + std::string(1, '\0') + args);
}

// 从快照 JSON 提取受管进程的 pid：定位 "\"managed\":true"，向前回溯最近的
// "\"pid\":" 取数字（Codec 的进程对象字段顺序固定，测试只依赖本仓实现）。
std::uint32_t firstManagedPid(const std::string& json) {
    const auto managed = json.find("\"managed\":true");
    if (managed == std::string::npos) return 0;
    const auto pidKey = json.rfind("\"pid\":", managed);
    if (pidKey == std::string::npos) return 0;
    return static_cast<std::uint32_t>(
        std::strtoul(json.c_str() + pidKey + 6, nullptr, 10));
}

// 遥测联动测试用捕获假件：单线程 tick 上下文写日志，无需加锁
// （与 daemon 审计同一线程假设）。
class CapturingLogger final : public foundation::ILogger {
public:
    void log(foundation::LogRecord record) override {
        records_.push_back(std::move(record));
    }
    void setLevel(foundation::LogLevel level) override { level_ = level; }
    foundation::LogLevel level() const override { return level_; }

    std::vector<foundation::LogRecord> drain() { return std::move(records_); }

private:
    foundation::LogLevel level_ = foundation::LogLevel::Debug;
    std::vector<foundation::LogRecord> records_;
};

// 剖面出口假件：清单报告句柄但字节不可读——验证回传对"清单与存储
// 分歧"的接口鲁棒性（daemon 对任意 ITickProfiler 实现安全，不推送
// 残缺帧）。
class UnreadableProfiler final : public theseed::runtime::ITickProfiler {
public:
    std::uint64_t trigger() override { return 0; }
    bool sampling() const override { return false; }
    std::vector<theseed::runtime::ITickProfiler::ArtifactMeta> listArtifacts()
        const override {
        theseed::runtime::ITickProfiler::ArtifactMeta phantom;
        phantom.handle = 77;
        phantom.tickCount = 4;
        return {phantom};
    }
    bool artifactPayload(std::uint64_t /*handle*/,
                         std::string& /*out*/) const override {
        return false;
    }
};

}  // namespace

int main() {
    std::cout << "MachineDaemonTest:" << std::endl;

    LocalHostProbe probe;
    auto supervisor = std::make_unique<LocalProcessSupervisor>();
    MachineAgent agent(std::make_unique<LocalHostProbe>(std::move(probe)),
                       std::move(supervisor));

    MachineDaemon::Config config;
    config.listenPort = 0;
    // execute 策略：仅本测试客户端来源 + 三个受控命令（安全缺省全拒，
    // 这里显式授权）；§6.1 角色绑定：客户端 = Admin（覆盖三档动作）
    config.execPolicy.trustedComponents = {kClientComponent};
    config.execPolicy.allowedCommands = {"start", "stop", "restart"};
    config.roleBindings = {{kClientComponent, AccessRole::Admin}};
    // 审计聚合只接 auditSink（不接 reportSink）：顺带覆盖 auditSink-only
    // 部署——采身份汇审计流，但不上注册簿。
    OpsControlCenter auditCenter;
    config.auditSink = &auditCenter;
    MachineDaemon daemon(config, agent);

    TEST("daemon starts, is idempotent, and reports its port");
    if (!daemon.start()) FAIL("daemon start failed");
    if (!daemon.start()) FAIL("second start must be idempotent-true");
    if (!daemon.isListening() || daemon.localPort() == 0)
        FAIL("daemon not listening after start");
    PASS();

    const auto port = daemon.localPort();
    auto daemonTick = [&daemon] { daemon.tick(); };

    RawClient client;
    TEST("client connects and settles");
    if (!client.connect(port)) FAIL("client connect failed");
    client.settle(daemonTick);
    PASS();

    TEST("machine.snapshot returns real host JSON");
    {
        RuntimeInvocation resp;
        if (!client.request(MachineMethod::kSnapshot, {}, daemonTick, resp))
            FAIL("no response to snapshot");
        if (resp.method != MachineMethod::kSnapshotOk)
            FAIL("wrong method: " + resp.method);
        const auto json = payloadToString(resp);
        if (json.find("\"hostname\":") == std::string::npos ||
            json.find("\"platform\":") == std::string::npos)
            FAIL("snapshot JSON missing host fields: " + json.substr(0, 120));
        PASS();
    }

    TEST("execute start spawns a managed child");
    {
        RuntimeInvocation resp;
        if (!client.request(MachineMethod::kExecute,
                            executePayload("start", "/bin/sleep 30"), daemonTick, resp))
            FAIL("no response to execute start");
        if (resp.method != MachineMethod::kExecuteOk)
            FAIL("wrong method: " + resp.method);
        if (resp.payload.size() != 1 || resp.payload[0] != std::byte{0x01})
            FAIL("execute start should report success");
        PASS();
    }

    TEST("snapshot shows the managed child with its pid");
    std::uint32_t childPid = 0;
    {
        RuntimeInvocation resp;
        if (!client.request(MachineMethod::kSnapshot, {}, daemonTick, resp))
            FAIL("no response to snapshot after start");
        const auto json = payloadToString(resp);
        if (json.find("\"managed\":true") == std::string::npos)
            FAIL("managed child missing from snapshot: " + json.substr(0, 120));
        childPid = firstManagedPid(json);
        if (childPid == 0) FAIL("failed to extract managed pid from snapshot");
        PASS();
    }

    TEST("execute stop terminates the child and clears managed state");
    {
        RuntimeInvocation resp;
        if (!client.request(MachineMethod::kExecute,
                            executePayload("stop", std::to_string(childPid)),
                            daemonTick, resp))
            FAIL("no response to execute stop");
        if (resp.method != MachineMethod::kExecuteOk ||
            resp.payload.size() != 1 || resp.payload[0] != std::byte{0x01})
            FAIL("execute stop should report success");

        if (!client.request(MachineMethod::kSnapshot, {}, daemonTick, resp))
            FAIL("no response to snapshot after stop");
        if (payloadToString(resp).find("\"managed\":true") != std::string::npos)
            FAIL("managed child should be gone after stop");
        PASS();
    }

    TEST("execute of unknown pid reports failure byte, not protocol error");
    {
        RuntimeInvocation resp;
        if (!client.request(MachineMethod::kExecute,
                            executePayload("restart", "4000000"), daemonTick, resp))
            FAIL("no response to execute restart");
        if (resp.method != MachineMethod::kExecuteOk)
            FAIL("expected execute.ok, got " + resp.method);
        if (resp.payload.size() != 1 || resp.payload[0] != std::byte{0x00})
            FAIL("unknown-pid restart should report failure byte");
        PASS();
    }

    TEST("malformed execute payload gets machine.error");
    {
        RuntimeInvocation resp;
        if (!client.request(MachineMethod::kExecute, payloadOf("start-no-separator"),
                            daemonTick, resp))
            FAIL("no response to malformed execute");
        if (resp.method != MachineMethod::kError) FAIL("expected machine.error");
        if (payloadToString(resp).find("NUL") == std::string::npos)
            FAIL("error payload should explain the reason");
        PASS();
    }

    TEST("empty command gets machine.error");
    {
        RuntimeInvocation resp;
        if (!client.request(MachineMethod::kExecute, executePayload("", "args"),
                            daemonTick, resp))
            FAIL("no response to empty command");
        if (resp.method != MachineMethod::kError) FAIL("expected machine.error");
        PASS();
    }

    TEST("execute policy rejects untrusted source and non-allowed command");
    {
        // 非受信来源：第二组件身份直连（hub 由其首条请求自报注册）
        RawClient stranger;
        stranger.component = 2;
        if (!stranger.connect(port)) FAIL("stranger connect failed");
        stranger.settle(daemonTick);
        RuntimeInvocation resp;
        if (!stranger.request(MachineMethod::kExecute,
                              executePayload("start", "/bin/sleep 30"),
                              daemonTick, resp))
            FAIL("no response to stranger execute");
        if (resp.method != MachineMethod::kError)
            FAIL("expected machine.error for untrusted source");
        if (payloadToString(resp).find("not trusted") == std::string::npos)
            FAIL("rejection should name the untrusted source: " +
                 payloadToString(resp));

        // 受信来源 + 非白名单命令
        if (!client.request(MachineMethod::kExecute, executePayload("format", "c"),
                            daemonTick, resp))
            FAIL("no response to non-allowed command");
        if (resp.method != MachineMethod::kError)
            FAIL("expected machine.error for non-allowed command");
        if (payloadToString(resp).find("not allowed") == std::string::npos)
            FAIL("rejection should name the command: " + payloadToString(resp));

        // 拒绝照记审计（accepted=false），且来源可见
        const auto& audit = daemon.auditLog();
        if (audit.size() < 2 || audit[audit.size() - 2].accepted ||
            audit.back().accepted)
            FAIL("policy rejections must be audited as rejected");
        if (audit[audit.size() - 2].source != 2)
            FAIL("audit should record the untrusted source component");
        PASS();
    }

    TEST("execute policy default denies everything");
    {
        // 缺省 ExecPolicy 两个白名单皆空：合法客户端也被拒（安全缺省）
        MachineAgent defaultAgent(std::make_unique<LocalHostProbe>(),
                                  std::make_unique<LocalProcessSupervisor>());
        MachineDaemon::Config defaultConfig;
        defaultConfig.listenPort = 0;
        MachineDaemon defaultDaemon(defaultConfig, defaultAgent);
        if (!defaultDaemon.start()) FAIL("default daemon start failed");
        auto defaultTick = [&defaultDaemon] { defaultDaemon.tick(); };

        RawClient defaultClient;
        if (!defaultClient.connect(defaultDaemon.localPort()))
            FAIL("default daemon connect failed");
        defaultClient.settle(defaultTick);

        RuntimeInvocation resp;
        if (!defaultClient.request(MachineMethod::kExecute,
                                   executePayload("start", "x"), defaultTick, resp))
            FAIL("no response under default policy");
        if (resp.method != MachineMethod::kError ||
            payloadToString(resp).find("not trusted") == std::string::npos)
            FAIL("default policy must reject execute, got: " +
                 payloadToString(resp));
        defaultDaemon.stop();
        PASS();
    }

    TEST("unknown method gets machine.error");
    {
        RuntimeInvocation resp;
        if (!client.request("machine.nope", {}, daemonTick, resp))
            FAIL("no response to unknown method");
        if (resp.method != MachineMethod::kError) FAIL("expected machine.error");
        if (payloadToString(resp).find("unknown method") == std::string::npos)
            FAIL("error payload should name the method");
        PASS();
    }

    TEST("machine.audit returns ordered trail of executes and rejects");
    {
        RuntimeInvocation resp;
        if (!client.request(MachineMethod::kAudit, {}, daemonTick, resp))
            FAIL("no response to audit");
        if (resp.method != MachineMethod::kAuditOk) FAIL("wrong method");
        const auto json = payloadToString(resp);
        // 成功 execute、失败 execute（unknown pid）、协议拒绝、未知方法各记一条
        if (json.find("\"command\":\"start\"") == std::string::npos ||
            json.find("\"accepted\":true,\"ok\":true") == std::string::npos)
            FAIL("successful start missing from audit: " + json.substr(0, 200));
        if (json.find("\"accepted\":true,\"ok\":false") == std::string::npos)
            FAIL("failed restart missing from audit");
        if (json.find("\"accepted\":false") == std::string::npos)
            FAIL("protocol rejects missing from audit");
        if (json.find("\"command\":\"machine.nope\"") == std::string::npos)
            FAIL("unknown method missing from audit");
        // 时间升序：start（首个受控命令）先于 machine.nope（最后一条）
        if (json.find("\"command\":\"start\"") > json.find("\"command\":\"machine.nope\""))
            FAIL("audit must be in chronological order");
        // 本地视图与 RPC 输出同源：start/stop（成功）、restart（失败）、
        // 畸形载荷、空命令、策略拒绝 ×2（非受信来源 + 非白名单命令）、
        // 未知方法 = 8 条
        if (daemon.auditLog().size() != 8)
            FAIL("expected 8 local audit entries, got " +
                 std::to_string(daemon.auditLog().size()));
        PASS();
    }

    TEST("execute audit entries flow to the ops center with node attribution");
    {
        const auto hostname = LocalHostProbe{}.sample().hostname;
        const auto trail = auditCenter.auditTrail();
        if (trail.size() != 8)
            FAIL("expected 8 forwarded entries, got " +
                 std::to_string(trail.size()));
        if (trail.front().nodeId != hostname)
            FAIL("entries must carry nodeId = snapshot hostname");
        if (trail.front().entry.command != "start" ||
            !trail.front().entry.accepted)
            FAIL("trail must be chronological starting with accepted start");
        bool sawUntrustedReject = false;
        for (const auto& record : trail) {
            if (!record.entry.accepted && record.entry.source == 2)
                sawUntrustedReject = true;
        }
        if (!sawUntrustedReject)
            FAIL("rejections must flow to center with source attribution");
        if (auditCenter.nodeCount() != 0)
            FAIL("audit-only wiring must not register the node");
        PASS();
    }

    TEST("telemetry linkage: metrics, logs, and traced execute");
    {
        // 指标为进程级单例：跨用例累积，按增量断言
        auto& accepted = foundation::MetricsRegistry::instance().counter(
            "machine_execute_accepted_count");
        auto& rejected = foundation::MetricsRegistry::instance().counter(
            "machine_execute_rejected_count");
        auto& durations = foundation::MetricsRegistry::instance().histogram(
            "machine_execute_duration_ms", {});
        const auto accepted0 = accepted.value();
        const auto rejected0 = rejected.value();
        const auto durationCount0 = durations.snapshot().count;

        const auto captured = std::make_shared<CapturingLogger>();
        auto previousLogger = foundation::takeGlobalLogger();
        foundation::setGlobalLogger(captured);
        // 捕获容器挂 shared_ptr：即便后续 FAIL 早退，发射器也不悬垂
        const auto spans = std::make_shared<std::vector<foundation::Span>>();
        foundation::setSpanEmitter(
            [spans](const foundation::Span& span) { spans->push_back(span); });

        RuntimeInvocation resp;
        // accepted 但执行失败（未知 pid）：不遗留子进程，ok=false 可观测
        if (!client.request(MachineMethod::kExecute,
                            executePayload("restart", "4000007"), daemonTick,
                            resp))
            FAIL("no response to traced execute");
        // rejected（非白名单命令）：拒绝同入指标/日志/trace
        if (!client.request(MachineMethod::kExecute,
                            executePayload("halt", "now"), daemonTick, resp))
            FAIL("no response to rejected execute");

        if (accepted.value() != accepted0 + 1) FAIL("accepted counter +1");
        if (rejected.value() != rejected0 + 1) FAIL("rejected counter +1");
        if (durations.snapshot().count != durationCount0 + 1)
            FAIL("execute duration observed once");

        if (spans->size() != 2) FAIL("one span per execute (accepted + rejected)");
        if ((*spans)[0].name != "machine.execute") FAIL("span name");
        if (!(*spans)[0].context.isValid()) FAIL("span carries trace context");
        bool sawOkFalse = false;
        for (const auto& attr : (*spans)[0].attrs) {
            if (attr.key == "ok" && std::get<bool>(attr.value) == false)
                sawOkFalse = true;
        }
        if (!sawOkFalse) FAIL("accepted span must carry ok=false here");
        bool sawRejected = false;
        for (const auto& attr : (*spans)[1].attrs) {
            if (attr.key == "accepted" && std::get<bool>(attr.value) == false)
                sawRejected = true;
        }
        if (!sawRejected) FAIL("rejected span must mark accepted=false");

        // 日志与 trace 自动关联：拒绝/执行日志各带其 span 的 traceId
        const auto records = captured->drain();
        const foundation::LogRecord* okLog = nullptr;
        const foundation::LogRecord* rejectLog = nullptr;
        for (const auto& record : records) {
            if (record.message == "machine.execute" && okLog == nullptr)
                okLog = &record;
            if (record.message == "machine.execute.rejected" &&
                rejectLog == nullptr)
                rejectLog = &record;
        }
        if (okLog == nullptr || okLog->traceId != (*spans)[0].context.traceId)
            FAIL("execute log must correlate with its span");
        if (rejectLog == nullptr ||
            rejectLog->traceId != (*spans)[1].context.traceId)
            FAIL("rejection log must correlate with its span");

        foundation::setSpanEmitter(nullptr);
        foundation::setGlobalLogger(std::move(previousLogger));
        PASS();
    }

    TEST("audit ring evicts oldest beyond capacity");
    {
        LocalHostProbe probe;
        MachineAgent smallAgent(
            std::make_unique<LocalHostProbe>(std::move(probe)),
            std::make_unique<LocalProcessSupervisor>());
        MachineDaemon::Config smallConfig;
        smallConfig.listenPort = 0;
        smallConfig.auditCapacity = 2;
        smallConfig.execPolicy.trustedComponents = {kClientComponent};
        smallConfig.execPolicy.allowedCommands = {"restart"};
        smallConfig.roleBindings = {{kClientComponent, AccessRole::Admin}};
        MachineDaemon smallDaemon(smallConfig, smallAgent);
        if (!smallDaemon.start()) FAIL("small daemon start failed");
        auto smallTick = [&smallDaemon] { smallDaemon.tick(); };

        RawClient smallClient;
        if (!smallClient.connect(smallDaemon.localPort())) FAIL("connect failed");
        smallClient.settle(smallTick);

        RuntimeInvocation resp;
        for (int i = 1; i <= 3; ++i) {
            if (!smallClient.request(MachineMethod::kExecute,
                                     executePayload("restart", "400000" + std::to_string(i)),
                                     smallTick, resp))
                FAIL("no response to execute " + std::to_string(i));
        }
        if (!smallClient.request(MachineMethod::kAudit, {}, smallTick, resp))
            FAIL("no response to small audit");
        const auto json = payloadToString(resp);
        if (json.find("4000001") != std::string::npos)
            FAIL("oldest entry must be evicted at capacity 2");
        if (json.find("4000002") == std::string::npos ||
            json.find("4000003") == std::string::npos)
            FAIL("newest two entries missing: " + json);
        smallDaemon.stop();
        PASS();
    }

    TEST("audit disabled with capacity 0");
    {
        LocalHostProbe probe;
        MachineAgent silentAgent(
            std::make_unique<LocalHostProbe>(std::move(probe)),
            std::make_unique<LocalProcessSupervisor>());
        MachineDaemon::Config silentConfig;
        silentConfig.listenPort = 0;
        silentConfig.auditCapacity = 0;
        silentConfig.execPolicy.trustedComponents = {kClientComponent};
        silentConfig.execPolicy.allowedCommands = {"restart"};
        silentConfig.roleBindings = {{kClientComponent, AccessRole::Admin}};
        // 本地环形关闭 ≠ 中心聚合关闭：auditSink 独立开关
        silentConfig.auditSink = &auditCenter;
        MachineDaemon silentDaemon(silentConfig, silentAgent);
        if (!silentDaemon.start()) FAIL("silent daemon start failed");
        auto silentTick = [&silentDaemon] { silentDaemon.tick(); };

        RawClient silentClient;
        if (!silentClient.connect(silentDaemon.localPort())) FAIL("connect failed");
        silentClient.settle(silentTick);

        RuntimeInvocation resp;
        // 遥测联动用例在其之前已追加过转发：按增量断言
        const auto forwarded0 = auditCenter.auditCount();
        if (!silentClient.request(MachineMethod::kExecute,
                                  executePayload("restart", "4000009"),
                                  silentTick, resp))
            FAIL("no response to execute");
        if (!silentClient.request(MachineMethod::kAudit, {}, silentTick, resp))
            FAIL("no response to audit");
        if (payloadToString(resp) != "[]") FAIL("disabled audit must be empty");
        if (!silentDaemon.auditLog().empty()) FAIL("local audit must stay empty");
        if (auditCenter.auditCount() != forwarded0 + 1)
            FAIL("local capacity 0 must not gate center forwarding");
        silentDaemon.stop();
        PASS();
    }

    TEST("daemon lifecycle registers with the ops center on start, deregisters on stop");
    {
        OpsControlCenter center;
        MachineAgent centerAgent(std::make_unique<LocalHostProbe>(),
                                 std::make_unique<LocalProcessSupervisor>(),
                                 &center);
        MachineDaemon::Config centerConfig;
        centerConfig.listenPort = 0;
        centerConfig.reportInterval = std::chrono::milliseconds{10};
        centerConfig.reportSink = &center;
        MachineDaemon centerDaemon(centerConfig, centerAgent);

        LocalHostProbe identityProbe;
        const auto hostname = identityProbe.sample().hostname;

        if (!centerDaemon.start()) FAIL("center daemon start failed");
        if (center.nodeCount() != 1) FAIL("start must register machine identity");
        NodeReport out;
        if (!center.latest(hostname, out))
            FAIL("nodeId must be the snapshot hostname");
        if (!out.summary.host.hostname.empty())
            FAIL("row starts as a registration placeholder");

        // 周期上报随后把占位行升级为快照（同键 upsert，不新增行）
        RawClient centerClient;
        if (!centerClient.connect(centerDaemon.localPort()))
            FAIL("center daemon connect failed");
        centerClient.settle([&centerDaemon] { centerDaemon.tick(); });
        if (center.nodeCount() != 1)
            FAIL("reporting upserts the registered row in place");
        if (!center.latest(hostname, out) ||
            out.summary.host.hostname != hostname)
            FAIL("placeholder must be upgraded to a real snapshot");

        centerDaemon.stop();
        if (center.nodeCount() != 0)
            FAIL("graceful stop deregisters immediately");
        PASS();
    }

    TEST("process governance: enumeration is policy-gated");
    {
        // 独立治理 daemon：来源白名单含本测试客户端；名单为空（只测枚举）
        OpsControlCenter governCenter;
        MachineAgent governAgent(std::make_unique<LocalHostProbe>(),
                                 std::make_unique<LocalProcessSupervisor>(),
                                 &governCenter);
        MachineDaemon::Config governConfig;
        governConfig.listenPort = 0;
        governConfig.auditSink = &governCenter;
        // execute 策略仅用于 spawn 一个受管 sleep：证明治理视图不重复
        // 暴露受管进程（受管编排走 execute，治理只看非受控）
        governConfig.execPolicy.trustedComponents = {kClientComponent};
        governConfig.execPolicy.allowedCommands = {"start", "stop"};
        governConfig.processGovernPolicy.trustedComponents = {kClientComponent};
        governConfig.roleBindings = {{kClientComponent, AccessRole::Admin}};
        MachineDaemon governDaemon(governConfig, governAgent);
        if (!governDaemon.start()) FAIL("govern daemon start failed");
        auto governTick = [&governDaemon] { governDaemon.tick(); };

        RawClient governClient;
        if (!governClient.connect(governDaemon.localPort()))
            FAIL("govern connect failed");
        governClient.settle(governTick);

        auto& listCount = foundation::MetricsRegistry::instance().counter(
            "machine_process_list_count");
        auto& listRejected = foundation::MetricsRegistry::instance().counter(
            "machine_process_list_rejected_count");
        const auto list0 = listCount.value();
        const auto listRejected0 = listRejected.value();
        const auto audits0 = governCenter.auditCount();

        // 受信来源：返回非受控进程 JSON 数组（含 daemon 自身进程，
        // 不含受管进程——先 spawn 一个受管 sleep 证明被过滤）
        RuntimeInvocation resp;
        if (!governClient.request(MachineMethod::kExecute,
                                  executePayload("start", "/bin/sleep 30"),
                                  governTick, resp))
            FAIL("no response to managed spawn");
        if (!governClient.request(MachineMethod::kSnapshot, {}, governTick,
                                  resp))
            FAIL("no response to managed snapshot");
        const auto managedPid = firstManagedPid(payloadToString(resp));
        if (managedPid == 0) FAIL("managed pid missing for filtering test");

        if (!governClient.request(MachineMethod::kProcesses, {}, governTick,
                                  resp))
            FAIL("no response to process enumeration");
        if (resp.method != MachineMethod::kProcessesOk)
            FAIL("wrong method: " + resp.method);
        const auto listing = payloadToString(resp);
        const auto ownPid = std::to_string(static_cast<std::uint32_t>(::getpid()));
        if (listing.find("\"pid\":" + ownPid) == std::string::npos)
            FAIL("enumeration must contain the daemon's own pid: " +
                 listing.substr(0, 120));
        if (listing.find("\"pid\":" + std::to_string(managedPid)) !=
            std::string::npos)
            FAIL("governance view must not repeat managed processes");
        if (listing.find("\"managed\"") != std::string::npos)
            FAIL("governance view must not carry managed flags");

        if (!governClient.request(MachineMethod::kExecute,
                                  executePayload("stop", std::to_string(managedPid)),
                                  governTick, resp))
            FAIL("no response to managed cleanup");

        // 非受信来源：拒绝照记审计与计数
        RawClient stranger;
        stranger.component = 2;
        if (!stranger.connect(governDaemon.localPort()))
            FAIL("govern stranger connect failed");
        stranger.settle(governTick);
        if (!stranger.request(MachineMethod::kProcesses, {}, governTick, resp))
            FAIL("no response to stranger enumeration");
        if (resp.method != MachineMethod::kError)
            FAIL("stranger enumeration must be rejected");
        if (payloadToString(resp).find("not trusted") == std::string::npos)
            FAIL("rejection should name the untrusted source: " +
                 payloadToString(resp));

        if (listCount.value() != list0 + 1) FAIL("list counter +1");
        if (listRejected.value() != listRejected0 + 1)
            FAIL("list rejected counter +1");
        // 两次枚举尝试（受信+非受信）都进中心审计环形；spawn/stop 的
        // execute 审计同环但与 list 断言无关，按捕获点切片扫描
        if (governCenter.auditCount() != audits0 + 4)
            FAIL("spawn/list/stop/stranger-list must all be audited: " +
                 std::to_string(governCenter.auditCount() - audits0));
        const auto trail = governCenter.auditTrail();
        bool sawAcceptedList = false;
        bool sawRejectedList = false;
        for (std::size_t i = audits0; i < trail.size(); ++i) {
            if (trail[i].entry.command != "process.list") continue;
            if (trail[i].entry.accepted) sawAcceptedList = true;
            else sawRejectedList = true;
        }
        if (!sawAcceptedList)
            FAIL("accepted listing must be audited");
        if (!sawRejectedList)
            FAIL("rejected listing must be audited as refused");
        governDaemon.stop();
        PASS();
    }

    TEST("process governance: terminate guards");
    {
        OpsControlCenter guardCenter;
        MachineAgent guardAgent(std::make_unique<LocalHostProbe>(),
                                std::make_unique<LocalProcessSupervisor>(),
                                &guardCenter);
        MachineDaemon::Config guardConfig;
        guardConfig.listenPort = 0;
        guardConfig.auditSink = &guardCenter;
        guardConfig.execPolicy.trustedComponents = {kClientComponent};
        guardConfig.execPolicy.allowedCommands = {"start", "stop"};
        // 处置名单故意不含 sleep：名称不匹配臂可达
        guardConfig.processGovernPolicy.trustedComponents = {kClientComponent};
        guardConfig.processGovernPolicy.killableNames = {"other"};
        guardConfig.roleBindings = {{kClientComponent, AccessRole::Admin}};
        MachineDaemon guardDaemon(guardConfig, guardAgent);
        if (!guardDaemon.start()) FAIL("guard daemon start failed");
        auto guardTick = [&guardDaemon] { guardDaemon.tick(); };

        RawClient guardClient;
        if (!guardClient.connect(guardDaemon.localPort()))
            FAIL("guard connect failed");
        guardClient.settle(guardTick);

        auto& termRejected = foundation::MetricsRegistry::instance().counter(
            "machine_terminate_rejected_count");
        const auto rejected0 = termRejected.value();

        RuntimeInvocation resp;
        // 1) 载荷畸形：空载荷与非数字 pid
        if (!guardClient.request(MachineMethod::kTerminate, {}, guardTick, resp))
            FAIL("no response to empty terminate");
        if (payloadToString(resp).find("malformed") == std::string::npos)
            FAIL("empty payload must be named malformed: " +
                 payloadToString(resp));
        if (!guardClient.request(MachineMethod::kTerminate,
                                 payloadOf("abc"), guardTick, resp))
            FAIL("no response to malformed terminate");
        if (payloadToString(resp).find("malformed") == std::string::npos)
            FAIL("malformed pid must be named: " + payloadToString(resp));
        // 2) 非受信来源
        RawClient stranger;
        stranger.component = 2;
        if (!stranger.connect(guardDaemon.localPort()))
            FAIL("guard stranger connect failed");
        stranger.settle(guardTick);
        if (!stranger.request(MachineMethod::kTerminate,
                              payloadOf("4194305"), guardTick, resp))
            FAIL("no response to stranger terminate");
        if (payloadToString(resp).find("not trusted") == std::string::npos)
            FAIL("stranger terminate must be untrusted: " +
                 payloadToString(resp));
        // 3) 主机上不存在的 pid（pid_max 上界外，恒 ESRCH 区间）
        if (!guardClient.request(MachineMethod::kTerminate,
                                 payloadOf("4194305"), guardTick, resp))
            FAIL("no response to unknown-pid terminate");
        if (payloadToString(resp).find("unknown pid") == std::string::npos)
            FAIL("unknown pid must be named: " + payloadToString(resp));
        // 4) 守卫自身：daemon 就是本测试进程，处置自己必须被拒
        if (!guardClient.request(MachineMethod::kTerminate,
                                 payloadOf(std::to_string(::getpid())),
                                 guardTick, resp))
            FAIL("no response to self terminate");
        if (payloadToString(resp).find("itself") == std::string::npos)
            FAIL("self target must be refused: " + payloadToString(resp));

        // 5) 受管进程：治理路径拒绝，唯一出口是 stop/restart
        if (!guardClient.request(MachineMethod::kExecute,
                                 executePayload("start", "/bin/sleep 30"),
                                 guardTick, resp))
            FAIL("no response to managed spawn");
        if (!guardClient.request(MachineMethod::kSnapshot, {}, guardTick, resp))
            FAIL("no response to managed snapshot");
        const auto managedPid = firstManagedPid(payloadToString(resp));
        if (managedPid == 0) FAIL("managed pid missing for guard test");
        if (!guardClient.request(MachineMethod::kTerminate,
                                 payloadOf(std::to_string(managedPid)),
                                 guardTick, resp))
            FAIL("no response to managed terminate");
        if (payloadToString(resp).find("managed process") == std::string::npos)
            FAIL("managed target must be refused: " + payloadToString(resp));
        if (!guardClient.request(MachineMethod::kExecute,
                                 executePayload("stop", std::to_string(managedPid)),
                                 guardTick, resp))
            FAIL("no response to managed cleanup");

        // 6) 名称不在处置名单：fork 未登记的 sleep，进程在但名单不含
        const pid_t sleeper = ::fork();
        if (sleeper == 0) {
            ::execl("/bin/sleep", "sleep", "30", static_cast<char*>(nullptr));
            ::_exit(127);
        }
        if (!guardClient.request(MachineMethod::kTerminate,
                                 payloadOf(std::to_string(sleeper)),
                                 guardTick, resp))
            FAIL("no response to unlisted terminate");
        if (payloadToString(resp).find("not in the kill list") ==
            std::string::npos)
            FAIL("unlisted name must be refused: " + payloadToString(resp));
        int status = 0;
        ::kill(sleeper, SIGTERM);
        ::waitpid(sleeper, &status, 0);

        if (termRejected.value() != rejected0 + 7)
            FAIL("all seven rejections must count, got delta " +
                 std::to_string(termRejected.value() - rejected0));
        guardDaemon.stop();
        PASS();
    }

    TEST("process governance: accepted disposition kills an unmanaged process");
    {
        OpsControlCenter killCenter;
        MachineAgent killAgent(std::make_unique<LocalHostProbe>(),
                               std::make_unique<LocalProcessSupervisor>(),
                               &killCenter);
        MachineDaemon::Config killConfig;
        killConfig.listenPort = 0;
        killConfig.auditSink = &killCenter;
        killConfig.processGovernPolicy.trustedComponents = {kClientComponent};
        killConfig.processGovernPolicy.killableNames = {"sleep"};
        killConfig.roleBindings = {{kClientComponent, AccessRole::Admin}};
        MachineDaemon killDaemon(killConfig, killAgent);
        if (!killDaemon.start()) FAIL("kill daemon start failed");
        auto killTick = [&killDaemon] { killDaemon.tick(); };

        RawClient killClient;
        if (!killClient.connect(killDaemon.localPort()))
            FAIL("kill connect failed");
        killClient.settle(killTick);

        // 捕获 span：处置是关键受控动作，接受路径必须带 ok/accepted
        const auto spans = std::make_shared<std::vector<foundation::Span>>();
        foundation::setSpanEmitter(
            [spans](const foundation::Span& span) { spans->push_back(span); });

        // 未登记 sleep 子进程：枚举可见，SIGTERM 处置生效
        const pid_t sleeper = ::fork();
        if (sleeper == 0) {
            ::execl("/bin/sleep", "sleep", "30", static_cast<char*>(nullptr));
            ::_exit(127);
        }
        const auto sleeperPid = std::to_string(static_cast<std::uint32_t>(sleeper));

        RuntimeInvocation resp;
        if (!killClient.request(MachineMethod::kProcesses, {}, killTick, resp))
            FAIL("no response to pre-kill enumeration");
        if (payloadToString(resp).find("\"pid\":" + sleeperPid) ==
            std::string::npos)
            FAIL("unmanaged sleeper must appear in the governance view");

        auto& termAccepted = foundation::MetricsRegistry::instance().counter(
            "machine_terminate_accepted_count");
        const auto accepted0 = termAccepted.value();
        if (!killClient.request(MachineMethod::kTerminate,
                                payloadOf(sleeperPid), killTick, resp))
            FAIL("no response to governed terminate");
        if (resp.method != MachineMethod::kTerminateOk)
            FAIL("wrong method: " + resp.method);
        if (resp.payload.size() != 1 || resp.payload[0] != std::byte{0x01})
            FAIL("governed terminate should report success");
        int status = 0;
        ::waitpid(sleeper, &status, 0);
        if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGTERM)
            FAIL("sleeper must die by SIGTERM");
        if (termAccepted.value() != accepted0 + 1) FAIL("accepted counter +1");

        if (spans->size() != 1) FAIL("one span per governed terminate");
        if ((*spans)[0].name != "machine.terminate") FAIL("span name");
        bool sawAccepted = false;
        bool sawOk = false;
        for (const auto& attr : (*spans)[0].attrs) {
            if (attr.key == "accepted" && std::get<bool>(attr.value))
                sawAccepted = true;
            if (attr.key == "ok" && std::get<bool>(attr.value))
                sawOk = true;
        }
        if (!sawAccepted || !sawOk)
            FAIL("accepted span must carry accepted/ok");

        const auto trail = killCenter.auditTrail();
        if (trail.empty() || trail.back().entry.command != "process.kill" ||
            !trail.back().entry.accepted || !trail.back().entry.ok)
            FAIL("accepted disposition must be audited with ok=true");
        if (trail.back().entry.args != sleeperPid)
            FAIL("audit must record the target pid");

        foundation::setSpanEmitter(nullptr);
        killDaemon.stop();
        PASS();
    }

    TEST("diagnostics profiling: trigger, produce, download, audit, rejections");
    {
        // 入口关闭（tickProfiler == nullptr，主 daemon 未配采样出口）：
        // machine.profile.* 视同未知方法——统一错误臂照记审计并作答。
        const auto closedAudits0 = daemon.auditLog().size();
        RuntimeInvocation resp;
        if (!client.request(MachineMethod::kProfileTrigger, {}, daemonTick, resp))
            FAIL("no response to closed-entry trigger");
        if (resp.method != MachineMethod::kError)
            FAIL("closed entry must answer the unified error");
        if (payloadToString(resp).find("unknown method") == std::string::npos)
            FAIL("closed entry must fall to unknown method: " +
                 payloadToString(resp));
        if (daemon.auditLog().size() != closedAudits0 + 1)
            FAIL("closed-entry attempt must still be audited");
        if (daemon.auditLog().back().command != "machine.profile.trigger")
            FAIL("closed-entry audit must name the method");

        // 入口打开的独立 daemon：窗口 4 tick 的 TickProfiler 挂到真实
        // TickScheduler（宿主接线形态），策略授权本测试客户端（组件 1）。
        OpsControlCenter profileCenter;
        MachineAgent profileAgent(std::make_unique<LocalHostProbe>(),
                                  std::make_unique<LocalProcessSupervisor>(),
                                  &profileCenter);
        TickScheduler profileScheduler(std::chrono::milliseconds{0});
        TickProfiler::Config profilerConfig;
        profilerConfig.windowTicks = 4;
        profilerConfig.maxArtifacts = 4;
        TickProfiler profiler(profilerConfig);
        profileScheduler.setObserver(&profiler);

        MachineDaemon::Config profileConfig;
        profileConfig.listenPort = 0;
        profileConfig.auditSink = &profileCenter;
        profileConfig.diagnosticsPolicy.canTrigger = {kClientComponent};
        profileConfig.diagnosticsPolicy.canAccess = {kClientComponent};
        profileConfig.roleBindings = {{kClientComponent, AccessRole::Admin}};
        profileConfig.tickProfiler = &profiler;
        MachineDaemon profileDaemon(profileConfig, profileAgent);
        if (!profileDaemon.start()) FAIL("profile daemon start failed");
        auto profileTick = [&profileDaemon] { profileDaemon.tick(); };

        RawClient profileClient;
        if (!profileClient.connect(profileDaemon.localPort()))
            FAIL("profile connect failed");
        profileClient.settle(profileTick);

        // stranger 身份（组件 2）：两条名单都不含它
        RawClient stranger;
        stranger.component = 2;
        if (!stranger.connect(profileDaemon.localPort()))
            FAIL("stranger connect failed");
        stranger.settle(profileTick);

        auto& triggerAccepted = foundation::MetricsRegistry::instance().counter(
            "machine_profile_trigger_accepted_count");
        auto& triggerRejected = foundation::MetricsRegistry::instance().counter(
            "machine_profile_trigger_rejected_count");
        auto& accessAccepted = foundation::MetricsRegistry::instance().counter(
            "machine_profile_access_accepted_count");
        auto& accessRejected = foundation::MetricsRegistry::instance().counter(
            "machine_profile_access_rejected_count");
        const auto trigAcc0 = triggerAccepted.value();
        const auto trigRej0 = triggerRejected.value();
        const auto accAcc0 = accessAccepted.value();
        const auto accRej0 = accessRejected.value();

        const auto audits0 = profileCenter.auditTrail().size();

        // 未授权三连：触发（写侧）与清单/下载（读侧）各自被拒，且
        // 鉴权先于句柄存在性（下载不泄漏句柄空间信息）。
        if (!stranger.request(MachineMethod::kProfileTrigger, {}, profileTick,
                              resp))
            FAIL("no response to stranger trigger");
        if (resp.method != MachineMethod::kError)
            FAIL("stranger trigger must be refused");
        if (payloadToString(resp).find("not authorized") == std::string::npos)
            FAIL("unauthorized trigger must say why: " + payloadToString(resp));
        if (!stranger.request(MachineMethod::kProfiles, {}, profileTick, resp))
            FAIL("no response to stranger listing");
        if (payloadToString(resp).find("not authorized") == std::string::npos)
            FAIL("unauthorized listing must say why: " + payloadToString(resp));
        if (!stranger.request(MachineMethod::kProfile, payloadOf("1"),
                              profileTick, resp))
            FAIL("no response to stranger download");
        if (payloadToString(resp).find("not authorized") == std::string::npos)
            FAIL("unauthorized download must say why: " +
                 payloadToString(resp));

        // 捕获 span：触发是关键受控动作（写侧性能预算），查询/下载不进
        // trace。置于 stranger 三连之后，只观察已授权侧的两次触发。
        const auto spans = std::make_shared<std::vector<foundation::Span>>();
        foundation::setSpanEmitter(
            [spans](const foundation::Span& span) { spans->push_back(span); });

        // 授权触发：句柄立即占号（窗口未收满也可被引用）
        if (!profileClient.request(MachineMethod::kProfileTrigger, {},
                                   profileTick, resp))
            FAIL("no response to authorized trigger");
        if (resp.method != MachineMethod::kProfileTriggerOk)
            FAIL("authorized trigger must succeed: " + payloadToString(resp));
        const auto handle = payloadToString(resp);
        if (handle.empty() || handle == "0") FAIL("handle must be nonzero");
        if (!profiler.sampling()) FAIL("window must be sampling after trigger");

        // 窗口进行中重复触发：限流拒绝
        if (!profileClient.request(MachineMethod::kProfileTrigger, {},
                                   profileTick, resp))
            FAIL("no response to retrigger");
        if (resp.method != MachineMethod::kError)
            FAIL("retrigger must be rate limited");
        if (payloadToString(resp).find("active window") == std::string::npos)
            FAIL("retrigger must name the reason: " + payloadToString(resp));

        // 真实调度器驱满窗口：4 次 runOnce 后固化产物
        for (int i = 0; i < 4; ++i) profileScheduler.runOnce();
        if (profiler.sampling()) FAIL("window must close after 4 ticks");

        if (!profileClient.request(MachineMethod::kProfiles, {}, profileTick,
                                   resp))
            FAIL("no response to artifact listing");
        if (resp.method != MachineMethod::kProfilesOk)
            FAIL("authorized listing must succeed: " + payloadToString(resp));
        const auto listing = payloadToString(resp);
        if (listing.find("\"handle\":" + handle) == std::string::npos ||
            listing.find("\"tick_count\":4") == std::string::npos)
            FAIL("listing must expose the completed artifact: " + listing);

        // 按句柄下载：只读产物字节（JSON 快照）
        if (!profileClient.request(MachineMethod::kProfile, payloadOf(handle),
                                   profileTick, resp))
            FAIL("no response to artifact download");
        if (resp.method != MachineMethod::kProfileOk)
            FAIL("authorized download must succeed: " + payloadToString(resp));
        const auto artifact = payloadToString(resp);
        if (artifact.find("\"handle\":" + handle) == std::string::npos ||
            artifact.find("\"window_ticks\":4") == std::string::npos ||
            artifact.find("\"samples\":[") == std::string::npos)
            FAIL("download must return the snapshot JSON: " + artifact);

        // 第二个窗口：句柄继续增长，清单多元素化（覆盖分隔符分支），
        // 旧产物不被顶掉（环形容量 4）
        if (!profileClient.request(MachineMethod::kProfileTrigger, {},
                                   profileTick, resp))
            FAIL("no response to second trigger");
        if (resp.method != MachineMethod::kProfileTriggerOk)
            FAIL("second window must open after the first closed: " +
                 payloadToString(resp));
        const auto handle2 = payloadToString(resp);
        if (handle2 == handle || handle2 == "0")
            FAIL("handles must grow: " + handle2);
        for (int i = 0; i < 4; ++i) profileScheduler.runOnce();
        if (!profileClient.request(MachineMethod::kProfiles, {}, profileTick,
                                   resp))
            FAIL("no response to second listing");
        const auto listing2 = payloadToString(resp);
        if (listing2.find("\"handle\":" + handle) == std::string::npos ||
            listing2.find("\"handle\":" + handle2) == std::string::npos)
            FAIL("second listing must keep both artifacts: " + listing2);
        if (!profileClient.request(MachineMethod::kProfile, payloadOf(handle2),
                                   profileTick, resp))
            FAIL("no response to second download");
        if (resp.method != MachineMethod::kProfileOk)
            FAIL("second download must succeed: " + payloadToString(resp));
        if (payloadToString(resp).find("\"handle\":" + handle2) ==
            std::string::npos)
            FAIL("second download must return its own artifact");

        // 未知句柄与畸形载荷：拒绝并具名
        if (!profileClient.request(MachineMethod::kProfile, payloadOf("99999"),
                                   profileTick, resp))
            FAIL("no response to unknown-handle download");
        if (payloadToString(resp).find("unknown handle") == std::string::npos)
            FAIL("unknown handle must be named: " + payloadToString(resp));
        if (!profileClient.request(MachineMethod::kProfile, payloadOf("abc"),
                                   profileTick, resp))
            FAIL("no response to malformed download");
        if (payloadToString(resp).find("malformed") == std::string::npos)
            FAIL("malformed handle must be named: " + payloadToString(resp));
        if (!profileClient.request(MachineMethod::kProfile, {}, profileTick,
                                   resp))
            FAIL("no response to empty download");
        if (payloadToString(resp).find("malformed") == std::string::npos)
            FAIL("empty payload must be malformed too: " +
                 payloadToString(resp));

        // 指标四路增量：触发受/拒 2/2，访问受/拒 4/5
        if (triggerAccepted.value() != trigAcc0 + 2)
            FAIL("trigger accepted must be +2");
        if (triggerRejected.value() != trigRej0 + 2)
            FAIL("trigger rejected must be +2");
        if (accessAccepted.value() != accAcc0 + 4)
            FAIL("access accepted must be +4");
        if (accessRejected.value() != accRej0 + 5)
            FAIL("access rejected must be +5");

        // 只有关键受控动作（触发）进 trace：接受一次 + 限流拒绝一次
        // （真实调度器 runOnce 的 tick span 也走全局出口，按名字过滤）
        std::vector<const foundation::Span*> triggerSpans;
        for (const auto& span : *spans) {
            if (span.name == "machine.profile.trigger") {
                triggerSpans.push_back(&span);
            }
        }
        if (triggerSpans.size() != 3)
            FAIL("only trigger attempts trace, got " +
                 std::to_string(triggerSpans.size()));
        bool sawAccepted = false;
        bool sawHandle = false;
        for (const auto& attr : triggerSpans[0]->attrs) {
            if (attr.key == "accepted" && std::get<bool>(attr.value))
                sawAccepted = true;
            if (attr.key == "handle" &&
                std::get<std::int64_t>(attr.value) ==
                    static_cast<std::int64_t>(std::stoll(handle)))
                sawHandle = true;
        }
        if (!sawAccepted || !sawHandle)
            FAIL("accepted span must carry accepted/handle");
        bool sawRejectedSpan = false;
        for (const auto& attr : triggerSpans[1]->attrs) {
            if (attr.key == "accepted" && !std::get<bool>(attr.value))
                sawRejectedSpan = true;
        }
        if (!sawRejectedSpan) FAIL("retrigger span must mark rejection");
        bool sawSecondAccept = false;
        for (const auto& attr : triggerSpans[2]->attrs) {
            if (attr.key == "accepted" && std::get<bool>(attr.value))
                sawSecondAccept = true;
        }
        if (!sawSecondAccept)
            FAIL("second trigger span must mark acceptance");

        // 审计入环（中心聚合 + 本地环形镜像）：13 条动作按发生序落账
        const auto& trail = profileCenter.auditTrail();
        if (trail.size() != audits0 + 13)
            FAIL("thirteen profile actions must be audited, got " +
                 std::to_string(trail.size() - audits0));
        struct Expect {
            const char* command;
            bool accepted;
        };
        const Expect expected[13] = {
            {"profiler.trigger", false},  {"profiler.list", false},
            {"profiler.download", false}, {"profiler.trigger", true},
            {"profiler.trigger", false},  {"profiler.list", true},
            {"profiler.download", true},  {"profiler.trigger", true},
            {"profiler.list", true},      {"profiler.download", true},
            {"profiler.download", false}, {"profiler.download", false},
            {"profiler.download", false}};
        for (std::size_t i = 0; i < 13; ++i) {
            const auto& entry = trail[audits0 + i].entry;
            if (entry.command != expected[i].command ||
                entry.accepted != expected[i].accepted)
                FAIL(std::string("audit slot ") + std::to_string(i) +
                     " mismatch: " + entry.command);
        }
        if (trail[audits0 + 3].entry.args != handle)
            FAIL("accepted trigger must record the handle");
        if (trail[audits0 + 7].entry.args != handle2)
            FAIL("second trigger must record its own handle");
        if (profileDaemon.auditLog().size() != 13)
            FAIL("local ring must mirror the same actions");

        foundation::setSpanEmitter(nullptr);
        profileDaemon.stop();
        PASS();
    }

    TEST("diagnostics relay: agent pushes artifacts, center queries and downloads");
    {
        // 中心侧配置：读侧授权本测试客户端；副本环形 8 份
        OpsControlCenter::Config centerCfg;
        centerCfg.maxProfileArtifacts = 8;
        centerCfg.profilePolicy.canAccess = {kClientComponent};
        centerCfg.roleBindings = {{kClientComponent, AccessRole::Admin}};
        OpsControlCenter relayCenter(centerCfg);

        // agent 持 reportSink（注册 + 周期上报），daemon 持 artifactSink
        // （产物回传）——同 一个中心实例，两条汇聚通道各自工作。
        MachineAgent relayAgent(std::make_unique<LocalHostProbe>(),
                                std::make_unique<LocalProcessSupervisor>(),
                                &relayCenter);
        TickScheduler relayScheduler(std::chrono::milliseconds{0});
        TickProfiler::Config relayProfilerConfig;
        relayProfilerConfig.windowTicks = 4;
        relayProfilerConfig.maxArtifacts = 2;  // 小环形：后续窗口逐出旧产物
        TickProfiler relayProfiler(relayProfilerConfig);
        relayScheduler.setObserver(&relayProfiler);

        MachineDaemon::Config relayConfig;
        relayConfig.listenPort = 0;
        relayConfig.reportSink = &relayCenter;
        relayConfig.reportInterval = std::chrono::milliseconds{30};
        relayConfig.auditSink = &relayCenter;
        relayConfig.artifactSink = &relayCenter;
        relayConfig.diagnosticsPolicy.canTrigger = {kClientComponent};
        relayConfig.diagnosticsPolicy.canAccess = {kClientComponent};
        relayConfig.roleBindings = {{kClientComponent, AccessRole::Admin}};
        relayConfig.tickProfiler = &relayProfiler;
        MachineDaemon relayDaemon(relayConfig, relayAgent);
        if (!relayDaemon.start()) FAIL("relay daemon start failed");
        auto relayTick = [&relayDaemon] { relayDaemon.tick(); };

        RawClient relayClient;
        if (!relayClient.connect(relayDaemon.localPort()))
            FAIL("relay connect failed");
        relayClient.settle(relayTick);

        // nodeId：daemon 上线即按快照 hostname 注册（06 口径）
        const auto roster = relayCenter.snapshotNodes();
        if (roster.size() != 1) FAIL("exactly one node expected");
        const auto& nodeId = roster.front().nodeId;

        // 中心侧读入口的审计/指标基线（agent 侧触发也推审计，按 command
        // 前缀 + nodeId 空判区分中心本地动作）
        const auto countLocal = [](const std::vector<NodeAuditEntry>& trail,
                                   const char* command, bool accepted) {
            std::size_t n = 0;
            for (const auto& record : trail) {
                if (record.nodeId.empty() &&
                    record.entry.command == command &&
                    record.entry.accepted == accepted) {
                    ++n;
                }
            }
            return n;
        };
        auto& queryAccepted =
            foundation::MetricsRegistry::instance().counter(
                "center_profile_query_accepted_count");
        auto& queryRejected =
            foundation::MetricsRegistry::instance().counter(
                "center_profile_query_rejected_count");
        auto& downloadAccepted =
            foundation::MetricsRegistry::instance().counter(
                "center_profile_download_accepted_count");
        auto& downloadRejected =
            foundation::MetricsRegistry::instance().counter(
                "center_profile_download_rejected_count");
        const auto qAcc0 = queryAccepted.value();
        const auto qRej0 = queryRejected.value();
        const auto dAcc0 = downloadAccepted.value();
        const auto dRej0 = downloadRejected.value();
        const auto qAccAudit0 =
            countLocal(relayCenter.auditTrail(), "center.profiler.query", true);
        const auto dRejAudit0 = countLocal(relayCenter.auditTrail(),
                                           "center.profiler.download", false);

        // 触发采样（agent 侧策略门）→ 真实调度器驱满窗口 → 产物固化
        RuntimeInvocation resp;
        if (!relayClient.request(MachineMethod::kProfileTrigger, {},
                                 relayTick, resp))
            FAIL("no response to relay trigger");
        if (resp.method != MachineMethod::kProfileTriggerOk)
            FAIL("relay trigger must succeed: " + payloadToString(resp));
        const auto handle = payloadToString(resp);
        for (int i = 0; i < 4; ++i) relayScheduler.runOnce();
        if (relayProfiler.sampling()) FAIL("window must close");

        // 窗口后的 daemon tick 推产物副本（payload + 元数据）给中心
        relayTick();

        // 中心持有副本：中心侧下载读到与 agent 侧同源字节
        const auto handleNum =
            static_cast<std::uint64_t>(std::stoll(handle));
        std::string centerPayload;
        if (!relayCenter.downloadProfileArtifact(kClientComponent, nodeId,
                                                 handleNum, centerPayload))
            FAIL("center must hold the relayed copy");
        std::string agentPayload;
        if (!relayProfiler.artifactPayload(handleNum, agentPayload))
            FAIL("agent must still hold its own copy");
        if (centerPayload != agentPayload)
            FAIL("center copy must be the relayed bytes");

        // 元数据经 report() 通道汇聚：latest 快照的 profiles 带窗口句柄
        //（先泵满一个上报周期：首报在触发前，剖面为空）
        for (int i = 0; i < 40; ++i) {
            relayTick();
            ::usleep(2000);
        }
        NodeReport latestReport;
        if (!relayCenter.latest(nodeId, latestReport))
            FAIL("node must have a latest report");
        bool sawMeta = false;
        for (const auto& meta : latestReport.profiles) {
            if (meta.handle == handleNum) sawMeta = true;
        }
        if (!sawMeta)
            FAIL("report channel must carry profile metas, got " +
                 std::to_string(latestReport.profiles.size()));

        // 中心侧按维度查询：进程维（nodeId）命中；entity 维无生产者，
        // 如实落空
        ProfileQuery byNode;
        byNode.nodeId = nodeId;
        auto rows = relayCenter.queryProfiles(kClientComponent, byNode);
        bool sawRow = false;
        for (const auto& row : rows) {
            if (row.meta.handle == handleNum) sawRow = true;
        }
        if (!sawRow) FAIL("query by node must surface the window");
        ProfileQuery byEntity;
        byEntity.entityId = "e-1";
        if (!relayCenter.queryProfiles(kClientComponent, byEntity).empty())
            FAIL("entity dimension has no producer: honest empty");

        // 未授权的中心侧读：查询空、下载拒，且各记审计与指标
        if (!relayCenter.queryProfiles(2, byNode).empty())
            FAIL("unauthorized query must be empty");
        if (relayCenter.downloadProfileArtifact(2, nodeId, handleNum,
                                                centerPayload))
            FAIL("unauthorized download must be refused");

        // 未知句柄的中心侧下载：如实拒
        if (relayCenter.downloadProfileArtifact(kClientComponent, nodeId,
                                                99999, centerPayload))
            FAIL("unknown handle must be refused at the center");

        if (queryAccepted.value() != qAcc0 + 2) FAIL("query accepted +2");
        if (queryRejected.value() != qRej0 + 1) FAIL("query rejected +1");
        if (downloadAccepted.value() != dAcc0 + 1) FAIL("download accepted +1");
        if (downloadRejected.value() != dRej0 + 2)
            FAIL("download rejected +2");
        if (countLocal(relayCenter.auditTrail(), "center.profiler.query",
                       true) != qAccAudit0 + 2)
            FAIL("query acceptances audited twice");
        if (countLocal(relayCenter.auditTrail(), "center.profiler.download",
                       false) != dRejAudit0 + 2)
            FAIL("both download rejections audited");

        // 产物环形逐出（agent 侧 maxArtifacts=2）：后续窗口把第一份挤出
        // ——已回传账本随之修剪（被逐出者不会复现）；中心副本不受影响
        //（副本环形是历史事实，独立于 agent 侧存储）。
        for (int w = 0; w < 2; ++w) {
            if (!relayClient.request(MachineMethod::kProfileTrigger, {},
                                     relayTick, resp))
                FAIL("no response to eviction-phase trigger");
            if (resp.method != MachineMethod::kProfileTriggerOk)
                FAIL("eviction-phase trigger must succeed");
            for (int i = 0; i < 4; ++i) relayScheduler.runOnce();
        }
        relayTick();  // 推送新产物 + 修剪已回传账本
        if (!relayCenter.downloadProfileArtifact(kClientComponent, nodeId,
                                                 handleNum, centerPayload))
            FAIL("center keeps its copy after agent-side eviction");

        relayDaemon.stop();
        PASS();
    }

    TEST("diagnostics relay: unreadable artifact frames are skipped safely");
    {
        OpsControlCenter::Config centerCfg;
        centerCfg.profilePolicy.canAccess = {kClientComponent};
        centerCfg.roleBindings = {{kClientComponent, AccessRole::Admin}};
        OpsControlCenter skipCenter(centerCfg);

        MachineAgent skipAgent(std::make_unique<LocalHostProbe>(),
                               std::make_unique<LocalProcessSupervisor>());
        UnreadableProfiler phantom;  // 清单有句柄、字节不可读
        MachineDaemon::Config skipConfig;
        skipConfig.listenPort = 0;
        skipConfig.auditSink = &skipCenter;  // 采样本机身份（不上注册簿）
        skipConfig.artifactSink = &skipCenter;
        skipConfig.tickProfiler = &phantom;
        MachineDaemon skipDaemon(skipConfig, skipAgent);
        if (!skipDaemon.start()) FAIL("skip daemon start failed");
        skipDaemon.tick();  // 回传循环遇不可读产物：跳过不推送残缺帧
        skipDaemon.stop();

        LocalHostProbe identityProbe;
        const auto skipNodeId = identityProbe.sample().hostname;
        ProfileQuery all;
        if (!skipCenter.queryProfiles(kClientComponent, all).empty())
            FAIL("unreadable frame must not merge into the index");
        std::string out;
        if (skipCenter.downloadProfileArtifact(kClientComponent, skipNodeId,
                                               77, out))
            FAIL("unreadable frame must not be stored");
        PASS();
    }

    TEST("access tiers: role gates layer over policy gates (04 §6.1)");
    {
        // 三级角色 × 三档动作的正反矩阵（角色绑定缺省空表 = 全拒，这里
        // 显式绑定）：组件 1 = ReadOnly（inspect 可用）；组件 5 =
        // Operator；组件 6 = Admin；组件 7 = 策略白名单内但未绑定角色
        // （角色缺失 = 未授权）；组件 2 = 策略外（既有策略拒绝语义不变，
        // 先策略后角色）。inspect = snapshot/audit/清单；operate =
        // execute/触发；administer = terminate。
        OpsControlCenter tierCenter;
        MachineAgent tierAgent(std::make_unique<LocalHostProbe>(),
                               std::make_unique<LocalProcessSupervisor>(),
                               &tierCenter);
        TickScheduler tierScheduler(std::chrono::milliseconds{0});
        TickProfiler::Config tierProfilerConfig;
        tierProfilerConfig.windowTicks = 4;
        tierProfilerConfig.maxArtifacts = 4;
        TickProfiler tierProfiler(tierProfilerConfig);
        tierScheduler.setObserver(&tierProfiler);

        MachineDaemon::Config tierConfig;
        tierConfig.listenPort = 0;
        tierConfig.auditSink = &tierCenter;
        tierConfig.execPolicy.trustedComponents = {1, 5, 6, 7};
        tierConfig.execPolicy.allowedCommands = {"restart"};
        tierConfig.processGovernPolicy.trustedComponents = {1, 5, 6, 7};
        tierConfig.processGovernPolicy.killableNames = {"sleep"};
        tierConfig.diagnosticsPolicy.canTrigger = {1, 5, 6, 7};
        tierConfig.diagnosticsPolicy.canAccess = {1, 5, 6, 7};
        tierConfig.tickProfiler = &tierProfiler;
        tierConfig.roleBindings = {
            {1, AccessRole::ReadOnly},
            {5, AccessRole::Operator},
            {6, AccessRole::Admin},
        };
        MachineDaemon tierDaemon(tierConfig, tierAgent);
        if (!tierDaemon.start()) FAIL("tier daemon start failed");
        auto tierTick = [&tierDaemon] { tierDaemon.tick(); };

        // 各身份一个连接（hub 按首条请求的 sourceComponent 自报注册）
        RawClient ro;      // ReadOnly
        ro.component = 1;
        if (!ro.connect(tierDaemon.localPort())) FAIL("ro connect failed");
        ro.settle(tierTick);
        RawClient op;      // Operator
        op.component = 5;
        if (!op.connect(tierDaemon.localPort())) FAIL("op connect failed");
        op.settle(tierTick);
        RawClient adm;     // Admin
        adm.component = 6;
        if (!adm.connect(tierDaemon.localPort())) FAIL("adm connect failed");
        adm.settle(tierTick);
        RawClient unbound;  // 策略内、角色未绑定
        unbound.component = 7;
        if (!unbound.connect(tierDaemon.localPort()))
            FAIL("unbound connect failed");
        unbound.settle(tierTick);
        RawClient stranger;  // 策略外
        stranger.component = 2;
        if (!stranger.connect(tierDaemon.localPort()))
            FAIL("stranger connect failed");
        stranger.settle(tierTick);

        auto& execAccepted = foundation::MetricsRegistry::instance().counter(
            "machine_execute_accepted_count");
        auto& execRejected = foundation::MetricsRegistry::instance().counter(
            "machine_execute_rejected_count");
        auto& termAccepted = foundation::MetricsRegistry::instance().counter(
            "machine_terminate_accepted_count");
        auto& termRejected = foundation::MetricsRegistry::instance().counter(
            "machine_terminate_rejected_count");
        auto& trigAccepted = foundation::MetricsRegistry::instance().counter(
            "machine_profile_trigger_accepted_count");
        auto& trigRejected = foundation::MetricsRegistry::instance().counter(
            "machine_profile_trigger_rejected_count");
        auto& accessAccepted = foundation::MetricsRegistry::instance().counter(
            "machine_profile_access_accepted_count");
        auto& accessRejected = foundation::MetricsRegistry::instance().counter(
            "machine_profile_access_rejected_count");
        auto& listRejected = foundation::MetricsRegistry::instance().counter(
            "machine_process_list_rejected_count");
        auto& snapshotRejected =
            foundation::MetricsRegistry::instance().counter(
                "machine_snapshot_rejected_count");
        auto& auditRejected = foundation::MetricsRegistry::instance().counter(
            "machine_audit_rejected_count");
        auto& snapshotCount = foundation::MetricsRegistry::instance().counter(
            "machine_snapshot_count");
        const auto execAcc0 = execAccepted.value();
        const auto execRej0 = execRejected.value();
        const auto termAcc0 = termAccepted.value();
        const auto termRej0 = termRejected.value();
        const auto trigAcc0 = trigAccepted.value();
        const auto trigRej0 = trigRejected.value();
        const auto accAcc0 = accessAccepted.value();
        const auto accRej0 = accessRejected.value();
        const auto listRej0 = listRejected.value();
        const auto snapRej0 = snapshotRejected.value();
        const auto audRej0 = auditRejected.value();
        const auto snapCnt0 = snapshotCount.value();

        RuntimeInvocation resp;
        // ReadOnly：inspect 面可用（快照 + 清单），operate/administer 全拒
        if (!ro.request(MachineMethod::kSnapshot, {}, tierTick, resp))
            FAIL("no response to ro snapshot");
        if (resp.method != MachineMethod::kSnapshotOk)
            FAIL("ReadOnly must inspect: " + resp.method);
        if (!ro.request(MachineMethod::kProfiles, {}, tierTick, resp))
            FAIL("no response to ro listing");
        if (resp.method != MachineMethod::kProfilesOk)
            FAIL("ReadOnly with canAccess must list: " + payloadToString(resp));
        if (!ro.request(MachineMethod::kExecute,
                        executePayload("restart", "4000021"), tierTick, resp))
            FAIL("no response to ro execute");
        if (payloadToString(resp).find("requires Operator role") ==
            std::string::npos)
            FAIL("ReadOnly execute must be refused by role: " +
                 payloadToString(resp));
        if (!ro.request(MachineMethod::kProfileTrigger, {}, tierTick, resp))
            FAIL("no response to ro trigger");
        if (payloadToString(resp).find("requires Operator role") ==
            std::string::npos)
            FAIL("ReadOnly trigger must be refused by role: " +
                 payloadToString(resp));
        if (!ro.request(MachineMethod::kTerminate, payloadOf("4194307"),
                        tierTick, resp))
            FAIL("no response to ro terminate");
        if (payloadToString(resp).find("requires Admin role") ==
            std::string::npos)
            FAIL("ReadOnly terminate must be refused by role: " +
                 payloadToString(resp));

        // 策略白名单内但未绑定角色：策略门放行，角色门拒绝
        if (!unbound.request(MachineMethod::kExecute,
                             executePayload("restart", "4000022"), tierTick,
                             resp))
            FAIL("no response to unbound execute");
        if (payloadToString(resp).find("requires Operator role") ==
            std::string::npos)
            FAIL("unbound execute must be refused by role: " +
                 payloadToString(resp));
        if (!unbound.request(MachineMethod::kTerminate, payloadOf("4194308"),
                             tierTick, resp))
            FAIL("no response to unbound terminate");
        if (payloadToString(resp).find("requires Admin role") ==
            std::string::npos)
            FAIL("unbound terminate must be refused by role: " +
                 payloadToString(resp));
        // 只读面的角色拒绝臂：枚举与剖面读在 canAccess/govern 白名单
        // 放行后仍需绑定 ReadOnly 及以上
        if (!unbound.request(MachineMethod::kProcesses, {}, tierTick, resp))
            FAIL("no response to unbound enumeration");
        if (payloadToString(resp).find("requires ReadOnly role") ==
            std::string::npos)
            FAIL("unbound enumeration must be refused by role: " +
                 payloadToString(resp));
        if (!unbound.request(MachineMethod::kProfiles, {}, tierTick, resp))
            FAIL("no response to unbound listing");
        if (payloadToString(resp).find("requires ReadOnly role") ==
            std::string::npos)
            FAIL("unbound listing must be refused by role: " +
                 payloadToString(resp));

        // Operator：触发可用，处置（administer）拒
        if (!op.request(MachineMethod::kProfileTrigger, {}, tierTick, resp))
            FAIL("no response to op trigger");
        if (resp.method != MachineMethod::kProfileTriggerOk)
            FAIL("Operator trigger must succeed: " + payloadToString(resp));
        const auto tierHandle = payloadToString(resp);
        for (int i = 0; i < 4; ++i) tierScheduler.runOnce();
        if (!op.request(MachineMethod::kProfiles, {}, tierTick, resp))
            FAIL("no response to op listing");
        if (resp.method != MachineMethod::kProfilesOk)
            FAIL("Operator listing must succeed: " + payloadToString(resp));
        if (!op.request(MachineMethod::kTerminate, payloadOf("4194309"),
                        tierTick, resp))
            FAIL("no response to op terminate");
        if (payloadToString(resp).find("requires Admin role") ==
            std::string::npos)
            FAIL("Operator terminate must be refused by role: " +
                 payloadToString(resp));
        // 剖面下载的角色拒绝臂：canAccess 放行 + 句柄真实存在，仍差角色位
        if (!unbound.request(MachineMethod::kProfile, payloadOf(tierHandle),
                             tierTick, resp))
            FAIL("no response to unbound download");
        if (payloadToString(resp).find("requires ReadOnly role") ==
            std::string::npos)
            FAIL("unbound download must be refused by role: " +
                 payloadToString(resp));

        // Admin：execute（accept + ok=false，不留子进程）、清单、真实处置
        if (!adm.request(MachineMethod::kExecute,
                         executePayload("restart", "4000023"), tierTick, resp))
            FAIL("no response to adm execute");
        if (resp.method != MachineMethod::kExecuteOk ||
            resp.payload[0] != std::byte{0x00})
            FAIL("Admin execute must pass the role gate");
        if (!adm.request(MachineMethod::kProfiles, {}, tierTick, resp))
            FAIL("no response to adm listing");
        if (resp.method != MachineMethod::kProfilesOk)
            FAIL("Admin listing must succeed: " + payloadToString(resp));
        const pid_t sleeper = ::fork();
        if (sleeper == 0) {
            ::execl("/bin/sleep", "sleep", "30", static_cast<char*>(nullptr));
            ::_exit(127);
        }
        if (!adm.request(MachineMethod::kTerminate,
                         payloadOf(std::to_string(static_cast<std::uint32_t>(
                             sleeper))),
                         tierTick, resp))
            FAIL("no response to adm terminate");
        if (resp.method != MachineMethod::kTerminateOk ||
            resp.payload[0] != std::byte{0x01})
            FAIL("Admin terminate must pass the role gate: " +
                 payloadToString(resp));
        int status = 0;
        ::waitpid(sleeper, &status, 0);
        if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGTERM)
            FAIL("sleeper must die by SIGTERM");

        // 策略先行语义保持：策略外来源的拒绝消息仍是既有口径（先策略
        // 后角色，拒绝原因不失真）
        if (!stranger.request(MachineMethod::kExecute,
                              executePayload("restart", "4000024"), tierTick,
                              resp))
            FAIL("no response to stranger execute");
        if (payloadToString(resp).find("not trusted") == std::string::npos)
            FAIL("stranger execute must be refused by policy: " +
                 payloadToString(resp));
        // 只读面无策略门：角色门是唯一关口（未绑定 = 拒绝，且照记审计）
        if (!stranger.request(MachineMethod::kSnapshot, {}, tierTick, resp))
            FAIL("no response to stranger snapshot");
        if (payloadToString(resp).find("requires ReadOnly role") ==
            std::string::npos)
            FAIL("unbound snapshot must be refused by role: " +
                 payloadToString(resp));
        if (!stranger.request(MachineMethod::kAudit, {}, tierTick, resp))
            FAIL("no response to stranger audit query");
        if (payloadToString(resp).find("requires ReadOnly role") ==
            std::string::npos)
            FAIL("unbound audit query must be refused by role: " +
                 payloadToString(resp));

        // 指标增量：角色拒绝进各动作族既有拒绝计数（同款口径），只读
        // 面拒绝进补齐成对的 snapshot/audit 拒绝计数
        if (execAccepted.value() != execAcc0 + 1) FAIL("exec accepted +1");
        if (execRejected.value() != execRej0 + 3)
            FAIL("exec rejected +3 (ro/unbound/stranger)");
        if (termAccepted.value() != termAcc0 + 1) FAIL("terminate accepted +1");
        if (termRejected.value() != termRej0 + 3)
            FAIL("terminate rejected +3 (ro/unbound/op)");
        if (trigAccepted.value() != trigAcc0 + 1) FAIL("trigger accepted +1");
        if (trigRejected.value() != trigRej0 + 1) FAIL("trigger rejected +1");
        if (accessAccepted.value() != accAcc0 + 3)
            FAIL("access accepted +3 (ro/op/adm listings)");
        if (accessRejected.value() != accRej0 + 2)
            FAIL("access rejected +2 (unbound listing/download)");
        if (listRejected.value() != listRej0 + 1)
            FAIL("list rejected +1 (unbound enumeration)");
        if (snapshotRejected.value() != snapRej0 + 1)
            FAIL("snapshot rejected +1");
        if (auditRejected.value() != audRej0 + 1) FAIL("audit rejected +1");
        if (snapshotCount.value() != snapCnt0 + 1) FAIL("snapshot served +1");

        // 审计：18 次动作留痕（3 次接受的清单 + 全部拒绝与接受的控制
        // 动作；接受的 snapshot/audit 查询不留痕避免噪声）
        const auto& trail = tierDaemon.auditLog();
        if (trail.size() != 18)
            FAIL("all eighteen audited actions must be present, got " +
                 std::to_string(trail.size()));
        bool sawRoleRejectSnapshot = false;
        bool sawAcceptedKill = false;
        for (const auto& entry : trail) {
            if (entry.command == "machine.snapshot" && !entry.accepted)
                sawRoleRejectSnapshot = true;
            if (entry.command == "process.kill" && entry.accepted && entry.ok)
                sawAcceptedKill = true;
        }
        if (!sawRoleRejectSnapshot)
            FAIL("role-rejected snapshot must be audited");
        if (!sawAcceptedKill) FAIL("Admin disposition must be audited");

        tierDaemon.stop();
        PASS();
    }

    TEST("config hot-change: whitelist applies, protected classes refused (04 §6.3)");
    {
        // Admin 级热改通道：禁改四类命中即拒且指认类别；白名单外一律
        // 拒；白名单内生效且效果可观测（上报周期 0 = 关闭 → 1ms 打开，
        // 下一轮 tick 中心即见快照）。
        OpsControlCenter cfgCenter;
        MachineAgent cfgAgent(std::make_unique<LocalHostProbe>(),
                              std::make_unique<LocalProcessSupervisor>(),
                              &cfgCenter);
        MachineDaemon::Config cfgConfig;
        cfgConfig.listenPort = 0;
        cfgConfig.auditSink = &cfgCenter;
        cfgConfig.reportSink = &cfgCenter;
        cfgConfig.reportInterval = std::chrono::milliseconds{0};  // 上报关闭
        cfgConfig.roleBindings = {{kClientComponent, AccessRole::Admin},
                                  {5, AccessRole::Operator}};
        MachineDaemon cfgDaemon(cfgConfig, cfgAgent);
        if (!cfgDaemon.start()) FAIL("cfg daemon start failed");
        auto cfgTick = [&cfgDaemon] { cfgDaemon.tick(); };

        RawClient cfgClient;
        if (!cfgClient.connect(cfgDaemon.localPort())) FAIL("cfg connect failed");
        cfgClient.settle(cfgTick);
        RawClient op;
        op.component = 5;
        if (!op.connect(cfgDaemon.localPort())) FAIL("cfg op connect failed");
        op.settle(cfgTick);

        auto& applyAccepted = foundation::MetricsRegistry::instance().counter(
            "machine_config_apply_accepted_count");
        auto& applyRejected = foundation::MetricsRegistry::instance().counter(
            "machine_config_apply_rejected_count");
        const auto applyAcc0 = applyAccepted.value();
        const auto applyRej0 = applyRejected.value();

        // 捕获 span：配置变更是关键受控动作
        const auto spans = std::make_shared<std::vector<foundation::Span>>();
        foundation::setSpanEmitter(
            [spans](const foundation::Span& span) { spans->push_back(span); });

        RuntimeInvocation resp;
        // 畸形载荷（缺 NUL 分隔）
        if (!cfgClient.request(MachineMethod::kConfigApply,
                               payloadOf("ops.report_interval_ms"), cfgTick,
                               resp))
            FAIL("no response to malformed apply");
        if (payloadToString(resp).find("malformed") == std::string::npos)
            FAIL("missing separator must be named: " + payloadToString(resp));
        // 角色门：Operator 不可 apply（apply runtime config 需 Admin）
        if (!op.request(MachineMethod::kConfigApply,
                        executePayload("ops.report_interval_ms", "1"),
                        cfgTick, resp))
            FAIL("no response to op apply");
        if (payloadToString(resp).find("requires Admin role") ==
            std::string::npos)
            FAIL("Operator apply must be refused by role: " +
                 payloadToString(resp));
        // 禁改四类：命中即拒且指认类别
        const std::pair<const char*, const char*> forbidden[4] = {
            {"migration.batch_size", "migration semantics"},
            {"protocol.version", "protocol definition"},
            {"persistence.schema_version", "persistence schema"},
            {"entity.property.flags", "entity property flags"},
        };
        for (const auto& [key, className] : forbidden) {
            if (!cfgClient.request(MachineMethod::kConfigApply,
                                   executePayload(key, "1"), cfgTick, resp))
                FAIL(std::string("no response to forbidden apply: ") + key);
            if (payloadToString(resp).find(className) == std::string::npos)
                FAIL(std::string("rejection must name the class for ") + key +
                     ": " + payloadToString(resp));
        }
        // 白名单外任意键：一律拒（扩面须改码评审，配置自身开不了门）
        if (!cfgClient.request(MachineMethod::kConfigApply,
                               executePayload("server.threads", "4"), cfgTick,
                               resp))
            FAIL("no response to non-whitelisted apply");
        if (payloadToString(resp).find("not in the change whitelist") ==
            std::string::npos)
            FAIL("non-whitelisted key must be named: " + payloadToString(resp));
        // 白名单内但值非法
        if (!cfgClient.request(MachineMethod::kConfigApply,
                               executePayload("ops.report_interval_ms", ""),
                               cfgTick, resp))
            FAIL("no response to empty-value apply");
        if (payloadToString(resp).find("invalid value") == std::string::npos)
            FAIL("empty value must be refused: " + payloadToString(resp));
        if (!cfgClient.request(MachineMethod::kConfigApply,
                               executePayload("ops.report_interval_ms", "abc"),
                               cfgTick, resp))
            FAIL("no response to non-numeric apply");
        if (payloadToString(resp).find("invalid value") == std::string::npos)
            FAIL("non-numeric value must be refused: " + payloadToString(resp));

        // 白名单内：生效（指标 +9 拒绝在此之后断言，先打点前值）
        if (!cfgClient.request(MachineMethod::kConfigApply,
                               executePayload("ops.report_interval_ms", "1"),
                               cfgTick, resp))
            FAIL("no response to whitelisted apply");
        if (resp.method != MachineMethod::kConfigApplyOk)
            FAIL("whitelisted apply must succeed: " + payloadToString(resp));
        if (resp.payload.size() != 1 || resp.payload[0] != std::byte{0x01})
            FAIL("applied response must be the success byte");

        // 效果可观测：上报从关闭到打开——一次 tick 即向中心推快照
        cfgTick();
        NodeReport applied;
        const auto hostname = LocalHostProbe{}.sample().hostname;
        if (!cfgCenter.latest(hostname, applied) ||
            applied.summary.host.hostname != hostname)
            FAIL("applied interval must take effect on the next tick");

        if (applyAccepted.value() != applyAcc0 + 1) FAIL("apply accepted +1");
        if (applyRejected.value() != applyRej0 + 9)
            FAIL("apply rejected +9 (malformed/role/4 classes/out-of-list/2 bad values)");

        // 审计：10 次尝试全部留痕；接受条目记录 key=value
        const auto& trail = cfgDaemon.auditLog();
        if (trail.size() != 10)
            FAIL("all ten apply attempts must be audited, got " +
                 std::to_string(trail.size()));
        if (!trail.back().accepted || !trail.back().ok ||
            trail.back().args != "ops.report_interval_ms=1")
            FAIL("accepted apply must record key=value");

        // span：每次尝试一个（配置变更全程受控）
        std::size_t applySpans = 0;
        for (const auto& span : *spans) {
            if (span.name == "machine.config.apply") ++applySpans;
        }
        if (applySpans != 10)
            FAIL("one span per apply attempt, got " +
                 std::to_string(applySpans));

        foundation::setSpanEmitter(nullptr);
        cfgDaemon.stop();
        PASS();
    }

    TEST("second daemon on the same port fails to start");
    {
        MachineDaemon::Config conflicting;
        conflicting.listenHost = "127.0.0.1";
        conflicting.listenPort = port;
        MachineDaemon second(conflicting, agent);
        if (second.start()) FAIL("conflicting start should fail");
        if (second.isListening()) FAIL("failed daemon must not be listening");
    }
    PASS();

    TEST("stop is safe and tick becomes a no-op");
    daemon.stop();
    if (daemon.isListening()) FAIL("daemon still listening after stop");
    daemon.tick();  // 不得崩溃
    PASS();

    TEST("telemetry exports over the OpsServer /metrics endpoint");
    {
        // 指标是进程级单例且此前用例都按增量断言；导出文本是绝对值，
        // 这里清零从零计数，HTTP 侧才能按确切数字断言。
        foundation::MetricsRegistry::instance().reset();

        // 对应部署形态：ops 控制面宿主进程同进程持有中心聚合器与
        // OpsServer——注册簿/审计水位与 machine 执行指标一并从
        // /metrics 导出（05-telemetry 的 MVP 导出面）。
        OpsControlCenter exportCenter;
        MachineAgent exportAgent(std::make_unique<LocalHostProbe>(),
                                 std::make_unique<LocalProcessSupervisor>(),
                                 &exportCenter);
        MachineDaemon::Config exportConfig;
        exportConfig.listenPort = 0;
        exportConfig.execPolicy.trustedComponents = {kClientComponent};
        exportConfig.execPolicy.allowedCommands = {"restart"};
        exportConfig.roleBindings = {{kClientComponent, AccessRole::Admin}};
        exportConfig.reportSink = &exportCenter;
        exportConfig.auditSink = &exportCenter;
        MachineDaemon exportDaemon(exportConfig, exportAgent);
        if (!exportDaemon.start()) FAIL("export daemon start failed");
        auto exportTick = [&exportDaemon] { exportDaemon.tick(); };

        theseed::ops::ProcessInfo exportInfo;
        exportInfo.role = "MachineDaemon";
        theseed::ops::OpsInspector exportInspector(exportInfo);
        theseed::ops::OpsServer::Config httpConfig;
        httpConfig.port = 0;  // ephemeral
        theseed::ops::OpsServer httpServer(httpConfig, exportInspector);
        if (!httpServer.start()) FAIL("metrics endpoint start failed");
        const auto httpPort = httpServer.localPort();

        // 一笔 accepted（失败 pid，不留子进程）+ 一笔 rejected：两个
        // 计数器与直方图都观测到，导出值才可精确断言。
        RawClient exportClient;
        if (!exportClient.connect(exportDaemon.localPort()))
            FAIL("export daemon connect failed");
        exportClient.settle(exportTick);
        RuntimeInvocation resp;
        if (!exportClient.request(MachineMethod::kExecute,
                                  executePayload("restart", "4000011"),
                                  exportTick, resp))
            FAIL("no response to exported execute");
        if (!exportClient.request(MachineMethod::kExecute,
                                  executePayload("halt", "now"),
                                  exportTick, resp))
            FAIL("no response to exported rejection");

        auto metricsConn = TcpConnection::create();
        if (!metricsConn->connect("127.0.0.1", httpPort))
            FAIL("connect to metrics endpoint failed");
        std::string rx;
        metricsConn->setOnReceived([&rx](std::span<const std::byte> data) {
            rx.append(reinterpret_cast<const char*>(data.data()), data.size());
        });
        const std::string request = "GET /metrics HTTP/1.1\r\n\r\n";
        metricsConn->write(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(request.data()), request.size()));
        // 头部声明多少就等到多少：一次读全再断言（导出文本无分片语义）
        for (int i = 0; i < 400; ++i) {
            httpServer.tick();
            metricsConn->pump();
            const auto headerEnd = rx.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                const auto lengthKey = rx.find("Content-Length: ");
                if (lengthKey != std::string::npos) {
                    const auto bodyLength =
                        std::strtoul(rx.c_str() + lengthKey + 16, nullptr, 10);
                    if (rx.size() >= headerEnd + 4 + bodyLength) break;
                }
            }
            ::usleep(2000);
        }

        const auto headerEnd = rx.find("\r\n\r\n");
        if (headerEnd == std::string::npos) FAIL("no reply from /metrics");
        if (rx.compare(0, 12, "HTTP/1.0 200") != 0)
            FAIL("metrics endpoint must answer 200");
        if (rx.find("text/plain; version=0.0.4") == std::string::npos)
            FAIL("Prometheus exposition content-type missing");
        const auto body = rx.substr(headerEnd + 4);
        if (body.find("machine_execute_accepted_count 1") == std::string::npos)
            FAIL("accepted counter must export with its value: " + body);
        if (body.find("machine_execute_rejected_count 1") == std::string::npos)
            FAIL("rejected counter must export with its value: " + body);
        if (body.find("machine_execute_duration_ms_bucket{le=") ==
            std::string::npos)
            FAIL("execute histogram buckets must export: " + body);
        if (body.find("machine_execute_duration_ms_count 1") ==
            std::string::npos)
            FAIL("execute histogram count must export: " + body);
        if (body.find("machine_execute_duration_ms_sum") == std::string::npos)
            FAIL("execute histogram sum must export: " + body);
        if (body.find("ops_nodes_registered 1") == std::string::npos)
            FAIL("center roster gauge must export: " + body);
        if (body.find("ops_audit_entries 2") == std::string::npos)
            FAIL("center audit watermark must export: " + body);

        httpServer.stop();
        exportDaemon.stop();
        PASS();
    }

    std::cout << "\nAll MachineDaemon tests passed!" << std::endl;
    return 0;
}
