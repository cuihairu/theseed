#pragma once

#include <cstdint>
#include <string>

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
    HostSummary sample() override;

private:
    std::uint64_t previousIdleTicks_ = 0;
    std::uint64_t previousTotalTicks_ = 0;
    bool hasPreviousCpuSample_ = false;
};

}  // namespace theseed::control::machine
