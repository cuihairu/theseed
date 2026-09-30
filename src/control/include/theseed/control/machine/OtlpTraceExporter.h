#pragma once

#include "theseed/foundation/Tracing.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace theseed::control::machine {

// 控制面遥测的 OTLP trace 导出器（05-telemetry-and-debug §2.1 导出层）。
//
// 零第三方依赖：OTLP/HTTP JSON 报文手写编码（不引 OTel SDK / protobuf /
// abseil），HTTP 传输复用仓内 runtime::TcpConnection 做一次性阻塞 POST。
//
// 异步批量外发：发射钩子只把 span 入队（链式转发仍在发射线程同步完成），
// 后台 worker 线程按 batchSize 满批 / flushInterval 兜底周期取批编码 POST，
// 失败按 maxAttempts 有界重试；空队列不发包。队列满丢新并计 exportFailed。
//
// 数据外发默认关闭（Config.enabled = false）：必须显式开启且 endpoint
// 合法才安装（挂全局 SpanEmitter + 起 worker）；安装成功即打启动日志
// otlp.traces.export.enabled，其 data_scope 属性明示导出的数据范围
// （导出面与不导出面清单见 docs/design/5-access-and-control-plane/
// 05-telemetry-and-debug.md §2.1）。
//
// 生命周期：install()/uninstall() 按宿主生命周期配对使用（非并发安全，
// 宿主单线程调用）；安装时链式保留宿主既有发射器，卸载时还原并 join
// worker——排空残余队列后退出，卸载即同步收口点。
class OtlpTraceExporter final {
public:
    struct Config final {
        // 数据外发开关：默认关闭，必须显式置 true（硬要求①）。
        bool enabled = false;
        // OTLP/HTTP JSON endpoint：http://<IPv4字面量>[:port][/path]，
        // 端口缺省 4318（OTLP/HTTP 约定端口）、path 缺省 "/"。仓内
        // transport 只做 IPv4 直连，无 DNS 与 TLS——需域名/加密时在
        // endpoint 前置本地代理（文档同步口径）。
        std::string endpoint;
        // resource 属性 service.name。
        std::string serviceName = "theseed";
        // 单次 POST 超时。外发在后台 worker 进行，该超时即单次 POST 的
        // 阻塞上界；发射钩子只入队，不再随网络等待。
        std::chrono::milliseconds timeout{500};
        // 批量：单次 POST 携带的 span 数上限（install 钳 ≥1）。
        std::size_t batchSize = 32;
        // 不满批的兜底外发周期（自安装与每次取批起算；≤0 = 有 span 即发）。
        std::chrono::milliseconds flushInterval{1000};
        // 每批总尝试次数（含首试，install 钳 ≥1）。
        std::uint32_t maxAttempts = 3;
        // 相邻尝试间隔（重试退避）。
        std::chrono::milliseconds retryBackoff{50};
        // 异步队列上限（install 钳 ≥1）：满则丢新 span 并计 exportFailed
        // ——本地过载只计数不刷屏，传输失败才有 warn（文档同步口径）。
        std::size_t maxQueue = 4096;
    };

    // endpoint 解析结果（resolved target）。
    struct Target final {
        std::string host;
        std::uint16_t port = 0;
        std::string path;
    };

    // 传输结果：ok = 收到 2xx；status = 解析出的 HTTP 状态码（0 = 未
    // 收到响应：建连失败 / 对端先关 / 超时）。
    struct PostResult final {
        bool ok = false;
        int status = 0;
    };

    // 传输接缝：缺省实现 = runtime::TcpConnection 阻塞 POST；测试可
    // 注入假传输直接断言请求内容。签名（target, body, timeout）。
    using HttpPost =
        std::function<PostResult(const Target&, const std::string&,
                                 std::chrono::milliseconds)>;

    explicit OtlpTraceExporter(Config config, HttpPost post = HttpPost{});

    // 析构即卸载（含排空队列并 join worker）：全局发射器槽位绝不悬挂
    // 指向已亡实例的回调。
    ~OtlpTraceExporter();

    // 安装（幂等）：enabled=false 或 endpoint 非法（含 warn 日志）时
    // 返回 false 且不触碰全局发射器、不起 worker。重装（uninstall 后
    // 再 install）复位停机状态并重开 worker。
    bool install();
    void uninstall();
    bool installed() const noexcept;

    std::uint64_t exportedOk() const noexcept;
    std::uint64_t exportFailed() const noexcept;

    // endpoint 解析：非法返回 nullopt（install 前置校验与单测直测共用）。
    static std::optional<Target> resolveEndpoint(std::string_view url);

    // span（批）→ OTLP/HTTP JSON 请求体（resourceSpans 单资源 + spans
    // 数组；纯函数供导出面口径与单测直断言）。单 span 重载 = 1 元素批。
    static std::string encodeTraces(const foundation::Span& span,
                                    std::string_view serviceName);
    static std::string encodeTraces(const std::vector<foundation::Span>& spans,
                                    std::string_view serviceName);

    // 响应状态行解码：完整状态行返回状态码，未收全或非状态行形态返回 0。
    static int decodeHttpStatus(std::string_view response);

    // OTLP 公共阻塞传输（TcpConnection 一次性 POST）：trace/metrics 两
    // 导出器共用单实现，HttpPost 接缝的缺省绑定。
    static PostResult postViaTcp(const Target& target, const std::string& body,
                                 std::chrono::milliseconds timeout);

private:
    void onSpan(const foundation::Span& span);
    // 后台取批循环（install 起线程入口）：满批/周期到点取批、停机排空。
    void workerLoop();
    // 单批外发：编码 + 有界重试 + 计数/warn（draining = 停机排水不重试）。
    void sendBatch(const std::vector<foundation::Span>& batch, bool draining);

    Config config_;
    HttpPost post_;
    Target target_;
    bool installed_ = false;
    foundation::SpanEmitter previous_;
    std::atomic<std::uint64_t> exportedOk_{0};
    std::atomic<std::uint64_t> exportFailed_{0};

    // 异步队列与 worker：queue_ 只由 onSpan（入队侧）与 workerLoop
    // （取批侧）在 queueMutex_ 下触碰；stopping_ 由 uninstall 置位。
    std::mutex queueMutex_;
    std::condition_variable flushCv_;
    std::thread worker_;
    std::vector<foundation::Span> queue_;
    bool stopping_ = false;
    std::chrono::steady_clock::time_point lastFlushAt_{};
};

}  // namespace theseed::control::machine
