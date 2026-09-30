#include "theseed/control/machine/HostProbe.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
// GetIfTable2 是 Vista 起 API：显式声明版本下限（#ifndef 不覆盖调用方
// 既定值）。MIB_IF_TABLE2/GetIfTable2/FreeMibTable 声明本职在
// <netioapi.h>，其类型块整体套在 _WS2IPDEF_（ws2ipdef.h 的包含守卫）
// 之下——先 iphlpapi.h 时 netioapi 走 __IPHLPAPI_H__ 捷径分支、跳过
// ws2ipdef.h 自包含，类型块即被整段跳过。规范序：winsock2（先于
// windows.h 防 winsock 冲突）→ ws2tcpip → windows → iphlpapi → netioapi。
#ifndef WINVER
#define WINVER 0x0600
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x06000000  // NTDDI_VISTA
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#else
#include <unistd.h>
#if defined(__linux__)
#include <sys/sysinfo.h>
#elif defined(__APPLE__)
// macOS 探针胶合所需的系统头：顺序按 BSD 惯例（sys/types → sys/socket →
// ifaddrs/net/if），乱序会触发「storage size unknown」类编译错误。
// cstdlib 显式给 getloadavg、sys/socket 显式给 AF_LINK，不依赖传递包含。
#include <cstdlib>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <mach/mach.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#endif
#endif

namespace theseed::control::machine {

namespace probe_detail {

void accumulateLinkCounters(LinkCounters& total, const LinkCounters& row) {
    if (row.loopback) {
        return;  // 回环流量不计入物理口径（Linux "lo" / macOS IFF_LOOPBACK）
    }
    total.rxBytes += row.rxBytes;
    total.txBytes += row.txBytes;
}

std::pair<std::uint64_t, std::uint64_t> aggregateLinkCounters(std::span<const LinkCounters> rows) {
    LinkCounters total{};
    for (const auto& row : rows) {
        accumulateLinkCounters(total, row);
    }
    return {total.rxBytes, total.txBytes};
}

LinkCounters windowsLinkCounters(std::uint32_t ifType, std::uint64_t rxBytes,
                                 std::uint64_t txBytes) {
    // 回环以 RFC 2863 ifType 判据（SDK IF_TYPE_SOFTWARE_LOOPBACK 同值，
    // Windows 胶合处 static_assert 对照）；排除与求和仍走单份 accumulate。
    return {ifType == kIfTypeSoftwareLoopback, rxBytes, txBytes};
}

std::pair<std::uint64_t, std::uint64_t> splitCpuTicksApple(std::uint64_t user, std::uint64_t nice,
                                                           std::uint64_t system, std::uint64_t idle) {
    const std::uint64_t busyTicks = user + nice + system;
    return {idle, busyTicks + idle};
}

double usagePercent(std::uint64_t used, std::uint64_t total) {
    if (total == 0) {
        return 0.0;  // 分母防护：读数未就绪时不产生 NaN/inf
    }
    return static_cast<double>((static_cast<long double>(used) / static_cast<long double>(total)) * 100.0L);
}

}  // namespace probe_detail

namespace {

#ifdef _WIN32
// GetIfTable2 的表必须以 FreeMibTable 归还；RAII 包装保证早退路径不漏。
struct FreeMibTableDeleter {
    void operator()(MIB_IF_TABLE2* table) const noexcept { FreeMibTable(table); }
};
#endif

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
        const auto total = static_cast<std::uint64_t>(info.totalram) * info.mem_unit;
        const auto used = static_cast<std::uint64_t>(info.totalram - info.freeram) * info.mem_unit;
        return probe_detail::usagePercent(used, total);
    }
#elif defined(__APPLE__)
    // 口径与活动监视器一致：占用 = (active + wired + compressor) 页 × 页大小，
    // 总量取 hw.memsize；纯比值走 probe_detail::usagePercent（Linux 单测覆盖）。
    vm_statistics64_data_t vmStats{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    host_t host = mach_host_self();
    const kern_return_t status =
        host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<integer_t*>(&vmStats), &count);
    mach_port_deallocate(mach_task_self(), host);
    std::uint64_t totalBytes = 0;
    std::size_t sizeOfTotal = sizeof(totalBytes);
    if (status == KERN_SUCCESS &&  // LCOV_EXCL_BR_LINE macOS 胶合在 Linux 上不参编，臂由 CI macos job 运行
        ::sysctlbyname("hw.memsize", &totalBytes, &sizeOfTotal, nullptr, 0) == 0 &&  // LCOV_EXCL_BR_LINE
        totalBytes != 0) {  // LCOV_EXCL_BR_LINE
        const auto pageSize = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
        const std::uint64_t usedPages = static_cast<std::uint64_t>(vmStats.active_count) +
                                        static_cast<std::uint64_t>(vmStats.wire_count) +
                                        static_cast<std::uint64_t>(vmStats.compressor_page_count);
        return probe_detail::usagePercent(usedPages * pageSize, totalBytes);
    }
    return 0.0;  // LCOV_EXCL_LINE macOS 系统调用失败兜底，Linux 覆盖率不可见
#endif

#if defined(__linux__)
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
#else
    // sysconf(_SC_AVPHYS_PAGES) 是 Linux/glibc 扩展，Apple 头不提供；
    // macOS 分支在上方已全路径 return，此处仅为矩阵外其他 POSIX 兜底。
    return 0.0;
#endif
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
#elif defined(__APPLE__)
    // HOST_CPU_LOAD_INFO 给出四态累计 ticks；拆接口径走
    // probe_detail::splitCpuTicksApple（Linux 单测直接驱动该纯函数）。
    host_cpu_load_info_data_t cpuInfo{};
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    host_t host = mach_host_self();
    const kern_return_t status =
        host_statistics(host, HOST_CPU_LOAD_INFO, reinterpret_cast<integer_t*>(&cpuInfo), &count);
    // mach_host_self 返回 send right，必须归还，否则每采样泄漏一个端口引用。
    mach_port_deallocate(mach_task_self(), host);
    if (status != KERN_SUCCESS) {  // LCOV_EXCL_BR_LINE macOS 胶合在 Linux 上不参编，由 CI macos job 运行
        return false;              // LCOV_EXCL_LINE
    }
    const auto& ticks = cpuInfo.cpu_ticks;
    const auto [idle, total] = probe_detail::splitCpuTicksApple(
        ticks[CPU_STATE_USER], ticks[CPU_STATE_NICE], ticks[CPU_STATE_SYSTEM], ticks[CPU_STATE_IDLE]);
    idleTicks = idle;
    totalTicks = total;
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
    probe_detail::LinkCounters total{};

    std::string line;
    while (std::getline(input, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;  // 表头（含 '|'）或空行
        }

        // 接口名裁空白；聚合口径排除回环（与物理流量统计的目标一致）
        auto name = line.substr(0, colon);
        const auto nameBegin = name.find_first_not_of(" \t");
        if (nameBegin == std::string::npos) {  // LCOV_EXCL_BR_LINE 冒号前全空白的行在真实 /proc/net/dev 不存在（数据行恒有接口名）
            continue;                          // LCOV_EXCL_LINE
        }
        name = name.substr(nameBegin, name.find_last_not_of(" \t") - nameBegin + 1);

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

        // 回环只打标记、不在此处跳过——排除判据与求和走 macOS 共用的
        // probe_detail::accumulateLinkCounters 单份实现。
        probe_detail::accumulateLinkCounters(total, {name == "lo", rx, tx});
    }

    rxBytes = total.rxBytes;
    txBytes = total.txBytes;
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
#elif defined(__APPLE__)
    // AF_LINK 项即每网卡的链路计数器；回环以 IFF_LOOPBACK 标志识别，
    // 聚合口径与 Linux 共用 probe_detail::aggregateLinkCounters。
    std::vector<probe_detail::LinkCounters> rows;
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) {  // LCOV_EXCL_BR_LINE macOS 胶合在 Linux 上不参编，由 CI macos job 运行
        return {0, 0};                   // LCOV_EXCL_LINE
    }
    for (const ifaddrs* entry = interfaces; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_LINK) {
            continue;  // IPv4/IPv6 地址项等，链路计数只在 AF_LINK 项上
        }
        // 内核在 AF_LINK 项挂 64 位版 if_data64（旧 if_data 的 u_char 计数
        // 会回绕；node_exporter 等工具同款读法）。
        const auto* counters = reinterpret_cast<const if_data64*>(entry->ifa_data);
        if (counters == nullptr) {  // LCOV_EXCL_BR_LINE AF_LINK 项 ifa_data 恒非空，防御臂
            continue;               // LCOV_EXCL_LINE
        }
        rows.push_back({(entry->ifa_flags & IFF_LOOPBACK) != 0, counters->ifi_ibytes,
                        counters->ifi_obytes});
    }
    freeifaddrs(interfaces);
    return probe_detail::aggregateLinkCounters(rows);
#elif defined(_WIN32)
    // GetIfTable2 给 MIB_IF_ROW2 的 64 位八位组计数（旧 MIB_IFROW 的 32 位
    // dwInOctets 会回绕）；回环以 RFC 2863 ifType 判据（常量与 SDK 宏在此
    // 编译期对照），归一与聚合口径与 Linux/macOS 共用 probe_detail 单份实现。
    // Windows 胶合在 Linux 上不参编，由 CI windows leg 编译并端到端首验。
    static_assert(probe_detail::kIfTypeSoftwareLoopback == IF_TYPE_SOFTWARE_LOOPBACK,
                  "shared loopback ifType must match the SDK constant");
    MIB_IF_TABLE2* rawTable = nullptr;
    if (GetIfTable2(&rawTable) != NO_ERROR || rawTable == nullptr) {
        return {0, 0};  // 查询失败兜底：读数未就绪不产生异常值
    }
    std::unique_ptr<MIB_IF_TABLE2, FreeMibTableDeleter> table(rawTable);

    std::vector<probe_detail::LinkCounters> rows;
    rows.reserve(static_cast<std::size_t>(table->NumEntries));
    for (ULONG index = 0; index < table->NumEntries; ++index) {
        const MIB_IF_ROW2& row = table->Table[index];
        rows.push_back(probe_detail::windowsLinkCounters(row.Type, row.InOctets, row.OutOctets));
    }
    return probe_detail::aggregateLinkCounters(rows);
#else
    // 矩阵外其他平台兜底（Linux/macOS/Windows 之外的平台组合）
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
