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

#include <arpa/inet.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <variant>
#include <vector>

using theseed::control::machine::LocalHostProbe;
using theseed::control::machine::LocalProcessSupervisor;
using theseed::control::machine::MachineAgent;
using theseed::control::machine::MachineDaemon;
using theseed::control::machine::NodeReport;
using theseed::control::ops::OpsControlCenter;
namespace foundation = theseed::foundation;
// 命名空间不能 using-declare，用别名
namespace MachineMethod = theseed::control::machine::MachineMethod;
using theseed::runtime::ComponentId;
using theseed::runtime::NetworkTransport;
using theseed::runtime::RuntimeInvocation;
using theseed::runtime::SendResult;
using theseed::runtime::TcpConnection;

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
    // 这里显式授权）
    config.execPolicy.trustedComponents = {kClientComponent};
    config.execPolicy.allowedCommands = {"start", "stop", "restart"};
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
