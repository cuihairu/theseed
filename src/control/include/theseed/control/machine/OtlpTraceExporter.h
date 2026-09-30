#pragma once

#include "theseed/foundation/Tracing.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace theseed::control::machine {

// 控制面遥测的 OTLP trace 导出器（05-telemetry-and-debug §2.1 导出层）。
//
// 零第三方依赖：OTLP/HTTP JSON 报文手写编码（不引 OTel SDK / protobuf /
// abseil），HTTP 传输复用仓内 runtime::TcpConnection 做一次性阻塞 POST。
//
// 数据外发默认关闭（Config.enabled = false）：必须显式开启且 endpoint
// 合法才安装到全局 SpanEmitter；安装成功即打启动日志
// otlp.traces.export.enabled，其 data_scope 属性明示导出的数据范围
// （导出面与不导出面清单见 docs/design/5-access-and-control-plane/
// 05-telemetry-and-debug.md §2.1）。
//
// 生命周期：install()/uninstall() 按宿主生命周期配对使用，期间不与
// 其它组件抢全局发射器槽位；安装时链式保留宿主既有发射器（导出后
// 照常转发），卸载时还原。
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
        // 单次 POST 超时。span 在发射钩子里同步外发，该超时即发射点
        // 的阻塞上界（控制面 span 频率低——每 RPC 一笔——可接受）。
        std::chrono::milliseconds timeout{500};
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

    // 析构即卸载：全局发射器槽位绝不悬挂指向已亡实例的回调。
    ~OtlpTraceExporter();

    // 安装（幂等）：enabled=false 或 endpoint 非法（含 warn 日志）时
    // 返回 false 且不触碰全局发射器。
    bool install();
    void uninstall();
    bool installed() const noexcept;

    std::uint64_t exportedOk() const noexcept;
    std::uint64_t exportFailed() const noexcept;

    // endpoint 解析：非法返回 nullopt（install 前置校验与单测直测共用）。
    static std::optional<Target> resolveEndpoint(std::string_view url);

    // span → OTLP/HTTP JSON 请求体（resourceSpans 单 span 载荷；纯函数
    // 供导出面口径与单测直断言）。
    static std::string encodeTraces(const foundation::Span& span,
                                    std::string_view serviceName);

    // 响应状态行解码：完整状态行返回状态码，未收全或非状态行形态返回 0。
    static int decodeHttpStatus(std::string_view response);

    // OTLP 公共阻塞传输（TcpConnection 一次性 POST）：trace/metrics 两
    // 导出器共用单实现，HttpPost 接缝的缺省绑定。
    static PostResult postViaTcp(const Target& target, const std::string& body,
                                 std::chrono::milliseconds timeout);

private:
    void onSpan(const foundation::Span& span);

    Config config_;
    HttpPost post_;
    Target target_;
    bool installed_ = false;
    foundation::SpanEmitter previous_;
    std::atomic<std::uint64_t> exportedOk_{0};
    std::atomic<std::uint64_t> exportFailed_{0};
};

}  // namespace theseed::control::machine
