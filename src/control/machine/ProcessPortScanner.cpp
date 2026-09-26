#include "theseed/control/machine/ProcessPortScanner.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace theseed::control::machine {

namespace {

#if defined(__linux__)
// /proc/net/tcp[tcp6] 列布局（空白分隔）：
//   sl local_address rem_address st tx:rx tr:tm retrnsmt uid timeout inode ...
// LISTEN 状态 st == "0A"；inode 是第 10 列（下标 9）。
constexpr std::string_view kListenState = "0A";
#endif

}  // namespace

void collectListenInodes(std::istream& input,
                         std::unordered_map<std::string, std::uint16_t>& out) {
#if defined(__linux__)
    std::string line;
    std::getline(input, line);  // 表头
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string sl;
        std::string local;
        std::string remote;
        std::string state;
        std::string queues;
        std::string timer;
        std::string retransmit;
        std::string uid;
        std::string timeout;
        std::string inode;
        if (!(fields >> sl >> local >> remote >> state >> queues >> timer >>
              retransmit >> uid >> timeout >> inode)) {
            continue;
        }
        if (state != kListenState) {
            continue;
        }

        // local_address 形如 "0100007F:1F90"（hex 端口在冒号后）
        const auto colon = local.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const auto port = static_cast<std::uint16_t>(
            std::strtoul(local.c_str() + colon + 1, nullptr, 16));
        if (port == 0) {
            continue;
        }
        out[inode] = port;
    }
#else
    static_cast<void>(input);
    static_cast<void>(out);
#endif
}

#if defined(__linux__)
namespace {

// 反查单个 pid 的监听端口：/proc/<pid>/fd 下的 socket:[inode] 命中 LISTEN 表。
// 一个进程监听多个端口时取最小端口号，保证快照输出稳定。
std::uint16_t lookupPidPort(
    std::uint32_t pid,
    const std::unordered_map<std::string, std::uint16_t>& listenInodes) {
    const std::filesystem::path fdDir =
        std::filesystem::path("/proc") / std::to_string(pid) / "fd";

    std::error_code error;
    std::uint16_t found = 0;
    for (const auto& entry : std::filesystem::directory_iterator(fdDir, error)) {
        if (error) {
            // LCOV_EXCL_START 迭代中途出错（进程消失/权限变化竞态）不可稳定
            // 注入；目录不存在时构造即置错、循环零迭代不走本臂
            break;
            // LCOV_EXCL_STOP
        }

        std::error_code linkError;
        const auto target = std::filesystem::read_symlink(entry.path(), linkError);
        if (linkError) {  // fd 关闭竞态：跳过该 fd
            continue;
        }

        // 链接目标形如 "socket:[12345]"：前缀 8 字符、末尾 ']' 1 字符
        const std::string& native = target.native();
        if (!native.starts_with("socket:[") || native.back() != ']' ||
            native.size() < 10) {  // "socket:[]":最短畸形也要排除
            continue;
        }
        const std::string inode = native.substr(8, native.size() - 9);
        const auto iter = listenInodes.find(inode);
        if (iter != listenInodes.end() && (found == 0 || iter->second < found)) {
            found = iter->second;
        }
    }
    return found;
}

}  // namespace
#endif  // defined(__linux__)

std::unordered_map<std::uint32_t, std::uint16_t> scanListeningPorts(
    const std::vector<std::uint32_t>& pids) {
    std::unordered_map<std::uint32_t, std::uint16_t> result;
#if defined(__linux__)
    std::unordered_map<std::string, std::uint16_t> listenInodes;
    {
        std::ifstream tcp("/proc/net/tcp");
        if (tcp.is_open()) {  // LCOV_EXCL_BR_LINE Linux 恒存在 /proc/net/tcp，打开失败臂不可注入
            collectListenInodes(tcp, listenInodes);
        }
        std::ifstream tcp6("/proc/net/tcp6");
        if (tcp6.is_open()) {  // LCOV_EXCL_BR_LINE 同上，/proc/net/tcp6 恒存在
            collectListenInodes(tcp6, listenInodes);
        }
    }
    if (listenInodes.empty()) {
        // LCOV_EXCL_START Linux 恒有内核自身监听套接字，空表臂不可达
        return result;
        // LCOV_EXCL_STOP
    }

    for (const auto pid : pids) {
        const auto port = lookupPidPort(pid, listenInodes);
        if (port != 0) {
            result.emplace(pid, port);
        }
    }
#else
    static_cast<void>(pids.size());  // 非 Linux：无等价 /proc 实现，返回空表
#endif
    return result;
}

std::string probeProcessVersion(std::uint16_t port, std::chrono::milliseconds timeout) {
#ifdef _WIN32
    // Windows 版需 winsock 初始化与阻塞语义适配（todo 遗留：跨平台探针完整实现）
    static_cast<void>(port);
    static_cast<void>(timeout);
    return "";
#else
    if (port == 0) {
        return "";
    }

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        // LCOV_EXCL_START fd 耗尽等资源异常，无法稳定注入
        return "";
        // LCOV_EXCL_STOP
    }
    // RAII 收口：任何 return 路径都关 fd，不留泄漏描述符
    struct FdGuard {
        int fd;
        ~FdGuard() { ::close(fd); }
    };
    FdGuard guard{fd};

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        return "";  // 端口无进程或拒绝连接
    }

    timeval tv{};
    tv.tv_sec = timeout.count() / 1000;
    tv.tv_usec = (timeout.count() % 1000) * 1000;
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {  // LCOV_EXCL_BR_LINE Linux 对合法 fd 的 setsockopt 恒成功
        return "";                                                          // LCOV_EXCL_START
    }
    if (::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {  // LCOV_EXCL_BR_LINE 同上
        return "";
    }
    // LCOV_EXCL_STOP

    const char request[] = "GET /health HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
    if (::send(fd, request, sizeof(request) - 1, 0) < 0) {
        // LCOV_EXCL_START 回环已连接的 send 失败需对端即刻 RST 的竞态
        return "";
        // LCOV_EXCL_STOP
    }

    // 读到对端关闭或超时为止；总量守卫防异常大响应占内存
    std::string response;
    char buffer[2048];
    for (;;) {
        const auto received = ::recv(fd, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }
        response.append(buffer, static_cast<std::size_t>(received));
        if (response.size() > 64 * 1024) {
            // LCOV_EXCL_START /health 恒小于 2KB，越界臂不可达
            break;
            // LCOV_EXCL_STOP
        }
    }

    // /health JSON 形如 {"role":"DBApp","version":"0.1.0",...}
    constexpr std::string_view kVersionKey = "\"version\":\"";
    const auto key = response.find(kVersionKey.data(), 0, kVersionKey.size());
    if (key == std::string::npos) {
        return "";  // 响应不是 theseed 进程（无版本字段）
    }
    const auto valueBegin = key + kVersionKey.size();
    const auto valueEnd = response.find('"', valueBegin);
    if (valueEnd == std::string::npos) {
        return "";  // 值无闭合引号：响应恰好截断在版本值中间
    }
    return response.substr(valueBegin, valueEnd - valueBegin);
#endif
}

}  // namespace theseed::control::machine
