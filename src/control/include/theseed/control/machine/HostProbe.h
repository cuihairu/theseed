#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace theseed::control::machine {

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
