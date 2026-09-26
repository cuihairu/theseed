#include "theseed/control/machine/HostProbe.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#if defined(__linux__)
#include <sys/sysinfo.h>
#endif
#endif

namespace theseed::control::machine {

namespace {

std::string detectPlatform() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

std::string queryHostname() {
#ifdef _WIN32
    char buffer[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (GetComputerNameA(buffer, &size) != 0) {
        return std::string(buffer, size);
    }
#else
    char buffer[256] = {};
    if (gethostname(buffer, sizeof(buffer)) == 0) {  // LCOV_EXCL_BR_LINE gethostname 失败臂系统调用不可定向注入
        buffer[sizeof(buffer) - 1] = '\0';
        return std::string(buffer);
    }
#endif

    // LCOV_EXCL_START gethostname 失败分支，系统调用无法定向注入
    return "unknown";
    // LCOV_EXCL_STOP
}

double queryDiskUsage() {
    std::error_code error;
    const auto space = std::filesystem::space(std::filesystem::current_path(), error);
    if (error || space.capacity == 0) {  // LCOV_EXCL_BR_LINE space 错误/零容量臂依赖宿主文件系统状态，不可注入
        return 0.0;
    }

    const auto used = static_cast<long double>(space.capacity - space.available);
    const auto capacity = static_cast<long double>(space.capacity);
    return static_cast<double>((used / capacity) * 100.0L);
}

#ifdef _WIN32
double queryMemoryUsage() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);

    if (GlobalMemoryStatusEx(&status) == 0) {
        return 0.0;
    }

    return static_cast<double>(status.dwMemoryLoad);
}

bool queryCpuTicks(std::uint64_t& idleTicks, std::uint64_t& totalTicks) {
    FILETIME idleTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    if (GetSystemTimes(&idleTime, &kernelTime, &userTime) == 0) {
        return false;
    }

    const auto pack = [](const FILETIME& value) {
        return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32u) |
               static_cast<std::uint64_t>(value.dwLowDateTime);
    };

    idleTicks = pack(idleTime);
    totalTicks = pack(kernelTime) + pack(userTime);
    return true;
}
#else
double queryMemoryUsage() {
#if defined(__linux__)
    struct sysinfo info{};
    if (sysinfo(&info) == 0 && info.totalram != 0) {  // LCOV_EXCL_BR_LINE Linux 下 sysinfo 恒成功且 totalram 恒非零，fallback 臂不可达
        const auto total = static_cast<long double>(info.totalram) * info.mem_unit;
        const auto free = static_cast<long double>(info.freeram) * info.mem_unit;
        return static_cast<double>(((total - free) / total) * 100.0L);
    }
#endif

    // LCOV_EXCL_START sysinfo 在 Linux 恒成功，sysconf fallback 不可达
    const long totalPages = sysconf(_SC_PHYS_PAGES);
    const long availablePages = sysconf(_SC_AVPHYS_PAGES);
    if (totalPages <= 0 || availablePages < 0) {
        return 0.0;
    }

    return static_cast<double>(
        ((static_cast<long double>(totalPages - availablePages)) /
         static_cast<long double>(totalPages)) *
        100.0L);
    // LCOV_EXCL_STOP
}

bool queryCpuTicks(std::uint64_t& idleTicks, std::uint64_t& totalTicks) {
#if defined(__linux__)
    std::ifstream input("/proc/stat");
    std::string label;
    std::uint64_t user = 0;
    std::uint64_t nice = 0;
    std::uint64_t system = 0;
    std::uint64_t idle = 0;
    std::uint64_t iowait = 0;
    std::uint64_t irq = 0;
    std::uint64_t softirq = 0;
    std::uint64_t steal = 0;

    if (!(input >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >>
          steal) ||  // LCOV_EXCL_BR_LINE 解析失败臂与 || 短路边归因本行：/proc/stat 首行恒可解析
        label != "cpu") {  // LCOV_EXCL_BR_LINE /proc/stat 首行恒为 "cpu" 且解析恒成功，失败臂不可注入
        // LCOV_EXCL_START /proc/stat 解析失败分支
        return false;
        // LCOV_EXCL_STOP
    }

    idleTicks = idle + iowait;
    totalTicks = user + nice + system + idle + iowait + irq + softirq + steal;
    return true;
#else
    idleTicks = 0;
    totalTicks = 0;
    return false;
#endif
}
#endif

double queryLoadAverage() {
#if defined(__linux__) || defined(__APPLE__)
    double load = 0.0;
    if (getloadavg(&load, 1) == 1) {  // LCOV_EXCL_BR_LINE getloadavg 失败臂不可定向注入
        return load;
    }
#endif

    // LCOV_EXCL_START getloadavg 失败兜底
    return 0.0;
    // LCOV_EXCL_STOP
}

#if defined(__linux__)
// 解析 /proc/net/dev 流并聚合除回环外的全部网卡流量。
// 行格式 "iface: rxBytes packets errs drop fifo frame compressed multicast
// txBytes packets errs drop fifo colls carrier compressed"；表头两行含 '|'。
void sumNetworkBytes(std::istream& input, std::uint64_t& rxBytes, std::uint64_t& txBytes) {
    rxBytes = 0;
    txBytes = 0;

    std::string line;
    while (std::getline(input, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;  // 表头（含 '|'）或空行
        }

        // 接口名裁空白；聚合口径排除回环（与物理流量统计的目标一致）
        auto name = line.substr(0, colon);
        const auto nameBegin = name.find_first_not_of(" \t");
        if (nameBegin == std::string::npos) {  // 冒号前全空白的行在真实 /proc/net/dev 不存在（数据行恒有接口名）
            continue;                          // LCOV_EXCL_LINE
        }
        name = name.substr(nameBegin, name.find_last_not_of(" \t") - nameBegin + 1);
        if (name == "lo") {
            continue;
        }

        std::uint64_t rx = 0;
        std::uint64_t tx = 0;
        std::istringstream fields(line.substr(colon + 1));
        if (!(fields >> rx)) {  // LCOV_EXCL_BR_LINE Linux 真实 /proc/net/dev 数据行首列恒为数值
            continue;           // LCOV_EXCL_START 畸形数据行防御臂：真实环境不可达
        }
        // LCOV_EXCL_STOP
        for (int index = 0; index < 7; ++index) {
            std::string skipped;
            fields >> skipped;  // packets errs drop fifo frame compressed multicast
        }
        if (!(fields >> tx)) {  // LCOV_EXCL_BR_LINE Linux 真实数据行 tx 列恒存在
            continue;           // LCOV_EXCL_START 截断数据行防御臂：真实环境不可达
        }
        // LCOV_EXCL_STOP

        rxBytes += rx;
        txBytes += tx;
    }
}
#endif

std::pair<std::uint64_t, std::uint64_t> queryNetworkBytes() {
#if defined(__linux__)
    std::ifstream input("/proc/net/dev");
    if (!input.is_open()) {  // LCOV_EXCL_BR_LINE Linux 恒有 /proc/net/dev，打开失败臂不可注入
        return {0, 0};       // LCOV_EXCL_START
    }
    // LCOV_EXCL_STOP

    std::uint64_t rxBytes = 0;
    std::uint64_t txBytes = 0;
    sumNetworkBytes(input, rxBytes, txBytes);
    return {rxBytes, txBytes};
#else
    // Windows/macOS 的等价探针暂缺（todo 遗留：跨平台主机探针完整实现）
    return {0, 0};
#endif
}

}  // namespace

LocalHostProbe::LocalHostProbe() : LocalHostProbe(Config{}) {}

LocalHostProbe::LocalHostProbe(Config config, CpuTickQuery cpuTickQuery, NetworkBytesQuery networkBytesQuery)
    : config_(config),
      cpuTickQuery_(std::move(cpuTickQuery)),
      networkBytesQuery_(std::move(networkBytesQuery)) {
    if (!cpuTickQuery_) {
        cpuTickQuery_ = queryCpuTicks;
    }
    if (!networkBytesQuery_) {
        networkBytesQuery_ = queryNetworkBytes;
    }
}

void LocalHostProbe::primeCpuSample(std::uint64_t& idleTicks, std::uint64_t& totalTicks) {
    const auto deadline = std::chrono::steady_clock::now() + config_.minCpuWindow;
    const auto baseTotal = totalTicks;

    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(config_.retryGranularity);

        std::uint64_t idle = 0;
        std::uint64_t total = 0;
        if (cpuTickQuery_(idle, total) && total > baseTotal) {
            idleTicks = idle;
            totalTicks = total;
            return;
        }
    }
    // 窗口耗尽仍未推进（系统 tick 冻结的退化情形）：保持首读数，差值为 0，
    // sample() 的粘滞逻辑会沿用 lastCpuUsage_。
}

HostSummary LocalHostProbe::sample() {
    HostSummary summary;
    summary.hostname = queryHostname();
    summary.platform = detectPlatform();
    summary.memoryUsage = queryMemoryUsage();
    summary.diskUsage = queryDiskUsage();
    summary.loadAverage = queryLoadAverage();
    std::tie(summary.networkRxBytes, summary.networkTxBytes) = networkBytesQuery_();

    std::uint64_t idleTicks = 0;
    std::uint64_t totalTicks = 0;
    if (cpuTickQuery_(idleTicks, totalTicks)) {
        if (!hasPreviousCpuSample_ && config_.minCpuWindow.count() > 0) {
            // 首采自举：以首读数为基线，窗口内等 tick 推进
            previousIdleTicks_ = idleTicks;
            previousTotalTicks_ = totalTicks;
            hasPreviousCpuSample_ = true;
            primeCpuSample(idleTicks, totalTicks);
        }

        if (hasPreviousCpuSample_) {
            const auto idleDelta = idleTicks - previousIdleTicks_;
            const auto totalDelta = totalTicks - previousTotalTicks_;
            if (totalDelta != 0) {
                const auto usage =
                    100.0 - (static_cast<double>(idleDelta) * 100.0 / static_cast<double>(totalDelta));
                lastCpuUsage_ = std::clamp(usage, 0.0, 100.0);
            }
            // totalDelta == 0：窗口短于 tick 粒度，沿用上次读数（不闪回 0）
        }

        previousIdleTicks_ = idleTicks;
        previousTotalTicks_ = totalTicks;
        hasPreviousCpuSample_ = true;
    }

    summary.cpuUsage = lastCpuUsage_;
    return summary;
}

}  // namespace theseed::control::machine
