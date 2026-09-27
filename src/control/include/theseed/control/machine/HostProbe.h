#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <utility>

namespace theseed::control::machine {

// 探针的平台无关纯计算面：Linux/macOS 的 syscall 胶合层把原始读数整理后
// 汇入这里，聚合口径只有一份。全平台编译——Linux 单测直接调用驱动全分支
// （macOS 胶合本身本机不可运行，由 CI macos job 端到端首验）。
namespace probe_detail {

// 单个网卡的累计计数器（胶合层从 /proc/net/dev 或 getifaddrs 提取）。
struct LinkCounters {
    bool loopback = false;
    std::uint64_t rxBytes = 0;
    std::uint64_t txBytes = 0;
};

// RFC 2863 / IANA ifType 的软件回环接口类型号。Windows MIB_IF_ROW2.Type
// 与 SDK 的 IF_TYPE_SOFTWARE_LOOPBACK 同值（Windows 胶合里有 static_assert
// 对照 SDK 宏），Linux 单测对本字面量钉死，防止无意识改动漂移。
inline constexpr std::uint32_t kIfTypeSoftwareLoopback = 24;

// Windows 胶合的行归一：MIB_IF_ROW2.Type → 回环标记（判据常量上方单份），
// 64 位八位组计数原样透传；Linux 单测直测驱动 true/false 两臂。
LinkCounters windowsLinkCounters(std::uint32_t ifType, std::uint64_t rxBytes,
                                 std::uint64_t txBytes);

// 单行累加：非回环才计入 total。回环排除判据只在此实现一份，
// 流式（Linux 逐行解析）与批量（macOS 全表遍历）两种消费形态共用。
void accumulateLinkCounters(LinkCounters& total, const LinkCounters& row);

// 聚合除回环外的全部网卡流量 (rx 合计, tx 合计)——Linux 以接口名 "lo"、
// macOS 以 IFF_LOOPBACK 标志置位 loopback，同一份求和与排除逻辑。
std::pair<std::uint64_t, std::uint64_t> aggregateLinkCounters(std::span<const LinkCounters> rows);

// Apple host_cpu_load_info 的四态 ticks 拆成 (idle, total)：忙 =
// user+system+nice、idle = CPU_STATE_IDLE（与活动监视器同口径）。
std::pair<std::uint64_t, std::uint64_t> splitCpuTicksApple(std::uint64_t user, std::uint64_t nice,
                                                           std::uint64_t system, std::uint64_t idle);

// used/total 百分比（0-100 线性）；total==0 返回 0（内存/磁盘共用的
// 分母防护）。used>total 的异常读数不裁剪——上层展示按 [0,100] 处理。
double usagePercent(std::uint64_t used, std::uint64_t total);

}  // namespace probe_detail

struct HostSummary {
    std::string hostname;
    std::string platform;
    double cpuUsage = 0.0;
    double memoryUsage = 0.0;
    double diskUsage = 0.0;
    double loadAverage = 0.0;
    std::uint64_t networkRxBytes = 0;
    std::uint64_t networkTxBytes = 0;
};

class IHostProbe {
public:
    virtual ~IHostProbe() = default;  // LCOV_EXCL_LINE C++ ABI：trivial 虚析构是空体，gcc 不为其生成计数指令，D0/D1/D2 三符号变体恒 0（结构不可测）

    virtual HostSummary sample() = 0;
};

class LocalHostProbe final : public IHostProbe {
public:
    // CPU 采样稳定化参数：
    // - 首采自举窗口：sample() 首次调用没有基线读数，在窗口内轮询等 CPU tick
    //   推进后以窗口两端差值出读数，CLI 单次调用也能拿到真实使用率；
    // - 粒度：窗口内的轮询步长（生产 /proc/stat 的 tick 粒度通常为 10ms）。
    struct Config {
        std::chrono::milliseconds minCpuWindow{200};
        std::chrono::milliseconds retryGranularity{10};
    };

    // 注入点：生产实现读 /proc/stat（CPU）与 /proc/net/dev（网络），
    // 测试注入脚本化序列驱动分支。返回 false 表示查询不可用。
    using CpuTickQuery =
        std::function<bool(std::uint64_t& idleTicks, std::uint64_t& totalTicks)>;
    using NetworkBytesQuery =
        std::function<std::pair<std::uint64_t, std::uint64_t>()>;  // (rxBytes, txBytes)

    // 注：`Config config = {}` 形式的类内默认实参对带 NSDMI 的嵌套类型非法
    //（gcc：default member initializer required before the end of its
    // enclosing class），故默认配置走无参重载。
    LocalHostProbe();
    explicit LocalHostProbe(
        Config config,
        CpuTickQuery cpuTickQuery = {},
        NetworkBytesQuery networkBytesQuery = {});

    HostSummary sample() override;

private:
    // 首采自举：以首读数为基线，窗口内轮询直到 tick 推进并更新当前读数。
    void primeCpuSample(std::uint64_t& idleTicks, std::uint64_t& totalTicks);

    Config config_;
    CpuTickQuery cpuTickQuery_;
    NetworkBytesQuery networkBytesQuery_;
    std::uint64_t previousIdleTicks_ = 0;
    std::uint64_t previousTotalTicks_ = 0;
    bool hasPreviousCpuSample_ = false;
    // 上次有效读数：窗口太短 tick 未推进或查询失败时保持，不闪回 0。
    double lastCpuUsage_ = 0.0;
};

}  // namespace theseed::control::machine
