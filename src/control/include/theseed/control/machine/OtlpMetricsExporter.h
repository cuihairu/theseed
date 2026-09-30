#pragma once

#include "theseed/control/machine/OtlpTraceExporter.h"
#include "theseed/foundation/Metrics.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace theseed::control::machine {

// 控制面遥测的 OTLP metrics 导出器（05-telemetry-and-debug §2.2 导出
// 层；与 OtlpTraceExporter 同构的 metrics 半边，trace 半边见同族头文件）。
//
// 与 trace 的差异只在数据源与驱动模型：trace 是事件驱动（span 完结钩
// 子），metrics 是周期拉取——exportIfDue() 由宿主（MachineDaemon::tick）
// 按 Config.interval 周期调用，到期即 collect() 全量注册表快照并一次
// POST（counter → 累积 sum + isMonotonic、gauge → gauge、histogram →
// 累积直方图；bucketCounts 与仓内注册表同为累积语义）。
//
// 硬约束与 trace 半边同口径：零第三方依赖（OTLP/HTTP JSON 报文手写编
// 码，传输复用 runtime::TcpConnection）；数据外发默认关闭
// （Config.enabled = false，不显式开启零网络副作用）；endpoint 仅
// http://<IPv4字面量>[:port][/path]；失败丢弃 + 计数 + warn，不重试。
// 开启即打启动日志 otlp.metrics.export.enabled，其 data_scope 属性明
// 示导出的数据范围（清单见 docs/design/5-access-and-control-plane/
// 05-telemetry-and-debug.md §2.2）。
//
// OTLP 公共面（Target / PostResult / HttpPost 接缝 / resolveEndpoint /
// decodeHttpStatus / postViaTcp 阻塞传输）沿用 OtlpTraceExporter 单实现。
//
// 生命周期：install()/uninstall() 按宿主生命周期配对。install 只做校
// 验与启动声明，不触碰任何全局槽位（与 trace 的发射器钩子不同——
// metrics 是拉模型，无全局可拦截点）；析构兜底卸载。
class OtlpMetricsExporter final {
public:
    struct Config final {
        // 数据外发开关：默认关闭，必须显式置 true（硬要求①）。
        bool enabled = false;
        // OTLP/HTTP JSON endpoint：http://<IPv4字面量>[:port][/path]，
        // 端口缺省 4318（OTLP/HTTP 约定端口）、path 缺省 "/"（解析沿用
        // OtlpTraceExporter::resolveEndpoint 单口径）。
        std::string endpoint;
        // resource 属性 service.name。
        std::string serviceName = "theseed";
        // 单次 POST 超时：导出在 tick 上下文同步阻塞，该超时即每次
        // 导出的阻塞上界。
        std::chrono::milliseconds timeout{500};
        // 拉取周期（exportIfDue 的到期门）；≤0 = 每次调用都导出。
        std::chrono::milliseconds interval{5000};
    };

    explicit OtlpMetricsExporter(
        Config config,
        OtlpTraceExporter::HttpPost post = OtlpTraceExporter::HttpPost{});

    // 析构即卸载（install 只落日志与旗标，无全局槽位可悬挂，卸载即
    // 保证后续导出不再外发）。
    ~OtlpMetricsExporter();

    // 安装（幂等）：enabled=false 或 endpoint 非法（含 warn 日志）时
    // 返回 false 且不记启用日志；成功打 otlp.metrics.export.enabled
    // 启动日志（data_scope 明示导出范围）。
    bool install();
    void uninstall();
    bool installed() const noexcept;

    std::uint64_t exportedOk() const noexcept;
    std::uint64_t exportFailed() const noexcept;

    // 周期驱动（宿主 tick 调用）：距上次导出不足 interval 则空转返回
    // false；到期导出并返回导出是否发起。now 由调用方注入（测试可控
    // 时钟），缺省取当前时刻；首调用距 epoch 必然到期（与
    // MachineDaemon::reportIfDue 的「首个 tick 立即上报」同口径）。
    bool exportIfDue(std::chrono::steady_clock::time_point now =
                         std::chrono::steady_clock::now());

    // 立即导出一轮注册表快照（未安装/未开启返回 false，零网络副作用）。
    bool exportOnce();

    // 样本 → OTLP/HTTP JSON 请求体（resourceMetrics 单批载荷；纯函数
    // 供导出面口径与单测直断言）。时间统一记 now（timeUnixNano；
    // 注册表不记每指标起始时刻，startTimeUnixNano 缺省 0 由采集端
    // 按累计口径处理）。
    static std::string encodeMetrics(
        const std::vector<foundation::MetricsRegistry::Sample>& samples,
        std::string_view serviceName,
        std::chrono::system_clock::time_point now);

private:
    void onExport(const std::string& body);

    Config config_;
    OtlpTraceExporter::HttpPost post_;
    OtlpTraceExporter::Target target_;
    bool installed_ = false;
    std::chrono::steady_clock::time_point lastExportAt_{};
    std::atomic<std::uint64_t> exportedOk_{0};
    std::atomic<std::uint64_t> exportFailed_{0};
};

}  // namespace theseed::control::machine
