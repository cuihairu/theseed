#include "theseed/control/machine/HostProbe.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

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
    if (gethostname(buffer, sizeof(buffer)) == 0) {
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
    if (error || space.capacity == 0) {
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
    if (sysinfo(&info) == 0 && info.totalram != 0) {
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
          steal) ||
        label != "cpu") {
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
    if (getloadavg(&load, 1) == 1) {
        return load;
    }
#endif

    // LCOV_EXCL_START getloadavg 失败兜底
    return 0.0;
    // LCOV_EXCL_STOP
}

}  // namespace

HostSummary LocalHostProbe::sample() {
    HostSummary summary;
    summary.hostname = queryHostname();
    summary.platform = detectPlatform();
    summary.memoryUsage = queryMemoryUsage();
    summary.diskUsage = queryDiskUsage();
    summary.loadAverage = queryLoadAverage();

    std::uint64_t idleTicks = 0;
    std::uint64_t totalTicks = 0;
    if (queryCpuTicks(idleTicks, totalTicks)) {
        if (hasPreviousCpuSample_) {
            const auto idleDelta = idleTicks - previousIdleTicks_;
            const auto totalDelta = totalTicks - previousTotalTicks_;
            if (totalDelta != 0) {
                const auto usage =
                    100.0 - (static_cast<double>(idleDelta) * 100.0 / static_cast<double>(totalDelta));
                summary.cpuUsage = std::clamp(usage, 0.0, 100.0);
            }
        }

        previousIdleTicks_ = idleTicks;
        previousTotalTicks_ = totalTicks;
        hasPreviousCpuSample_ = true;
    }

    return summary;
}

}  // namespace theseed::control::machine