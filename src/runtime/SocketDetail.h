#pragma once

// 平台 socket 细节：Winsock2 与 POSIX sockets 的差异集中在此，
// TcpConnection / TcpListener 只面向统一接口编码。

#if defined(_WIN32)

#include <mutex>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace theseed::runtime::detail {

using SocketHandle = SOCKET;
using SockLen = int;
inline constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
inline constexpr int kSocketError = SOCKET_ERROR;
inline constexpr int kSendFlags = 0;

inline bool wouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }

// 非阻塞 connect 尚在进行中（Winsock 以 WSAEWOULDBLOCK 表达）。
inline bool connectInProgress() { return WSAGetLastError() == WSAEWOULDBLOCK; }

inline void setNonBlocking(SocketHandle s) {
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
}

// 与 POSIX 分支同语义（见彼处注释）：关 Nagle、ENOTCONN 可重试、
// 未决 connect 的 SO_ERROR 读取。macOS 首发的 ENOTCONN 窗口问题在
// Windows 侧同样存在于 WSAENOTCONN。
inline void enableNoDelay(SocketHandle s) {
    const int nodelay = 1;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
}

inline bool notConnectedYet() { return WSAGetLastError() == WSAENOTCONN; }

inline int socketPendingError(SocketHandle s) {
    int err = 0;
    SockLen len = sizeof(err);
    ::getsockopt(s, SOL_SOCKET, SO_ERROR,
                 reinterpret_cast<char*>(&err), &len);
    return err;
}

inline bool connectStillPending(int err) {
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
}

inline void closeSocket(SocketHandle s) { closesocket(s); }

// WSAStartup 必须在任何 socket() 之前执行。用 once 语义保证幂等，
// 并让 connect/listen 自行调用（socketEnsureInit），调用方无需记得
// 先做全局初始化——Linux 分支为空操作，行为不变。
inline void socketEnsureInit() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}

inline void socketGlobalInit() { socketEnsureInit(); }

// 不做真正的 WSACleanup：与 POSIX 分支的空操作对称，进程存续期内
// Winsock 保持初始化。若在此清理，once 语义会让后续的 globalInit 与
// connect/listen 的自愈初始化都变成无效操作，同进程的多轮 init/shutdown
// 配对（测试进程的常态）在第二轮起全部失效；测试进程退出时由操作
// 系统回收 Winsock 资源。
inline void socketGlobalShutdown() {}

}  // namespace theseed::runtime::detail

#else

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace theseed::runtime::detail {

using SocketHandle = int;
using SockLen = socklen_t;
inline constexpr SocketHandle kInvalidSocket = -1;
inline constexpr int kSocketError = -1;
#if defined(MSG_NOSIGNAL)
// 阻止向已断开的连接写数据时触发 SIGPIPE 杀死进程。
inline constexpr int kSendFlags = MSG_NOSIGNAL;
#else
inline constexpr int kSendFlags = 0;
#endif

// EINTR 视为可重试的暂时性阻塞，与 Winsock 轮询语义一致。
inline bool wouldBlock() {
    // Linux 上 EWOULDBLOCK 与 EAGAIN 同值，第二比较真臂不可达，豁免登记在下行行尾。
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;  // LCOV_EXCL_BR_LINE EWOULDBLOCK==EAGAIN 同值短路：第二比较真臂结构性不可达，其余 miss 边为条件块汇合副本
}

// 非阻塞 connect 尚在进行中（POSIX 以 EINPROGRESS 表达）。
inline bool connectInProgress() {
    // EWOULDBLOCK 与 EAGAIN 同值：末位比较真臂不可达，豁免登记在下两行行尾。
    return errno == EINPROGRESS || errno == EINTR || errno == EAGAIN ||  // LCOV_EXCL_BR_LINE 同值短路链：EAGAIN 假蕴含 EWOULDBLOCK 假，末位比较真臂不可达，miss 边为汇合副本
           errno == EWOULDBLOCK;  // LCOV_EXCL_BR_LINE EWOULDBLOCK==EAGAIN，真臂结构性不可达
}

inline void setNonBlocking(SocketHandle s) {
    const int flags = ::fcntl(s, F_GETFL, 0);
    ::fcntl(s, F_SETFL, flags | O_NONBLOCK);
}

// 关闭 Nagle：请求/响应式控制面在非阻塞 tick 循环里逐条写小包，
// macOS 回环的 Nagle×延迟 ACK 交互会把应答拖过测试的 tick 预算
// （Linux 较不敏感，属平台行为差异而非语义分支）。
inline void enableNoDelay(SocketHandle s) {
    const int nodelay = 1;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
}

// 非阻塞 connect 尚未落定时，send/recv 以 ENOTCONN 表达"未就绪"，
// 与 EAGAIN 同属可重试——不能据此判死连接（macOS 上握手完成前的
// 首发命中此窗口的概率远高于 Linux）。
inline bool notConnectedYet() { return errno == ENOTCONN; }

// 读取未决 connect 的落定结果：0 = 握手成功，其余为 SO_ERROR 原值
// （调用方判 EINPROGRESS 系错误是否仍在进行）。
inline int socketPendingError(SocketHandle s) {
    int err = 0;
    SockLen len = sizeof(err);
    ::getsockopt(s, SOL_SOCKET, SO_ERROR,
                 reinterpret_cast<char*>(&err), &len);
    return err;
}

// SO_ERROR 值是否表示握手仍在进行（对照 connectInProgress 的 errno 集）。
inline bool connectStillPending(int err) {
    // EWOULDBLOCK 与 EAGAIN 同值：末位比较真臂不可达，豁免登记在下两行行尾。
    // EINPROGRESS/EINTR/EAGAIN 的语义集由 TcpConnectionTest 直测锚定。
    return err == EINPROGRESS || err == EINTR || err == EAGAIN ||  // LCOV_EXCL_BR_LINE 同值短路链：EAGAIN 真蕴含 EWOULDBLOCK 真，末位比较真臂不可达，miss 边为汇合副本
           err == EWOULDBLOCK;  // LCOV_EXCL_BR_LINE EWOULDBLOCK==EAGAIN，真臂结构性不可达
}

inline void closeSocket(SocketHandle s) { ::close(s); }

inline void socketEnsureInit() {}
inline void socketGlobalInit() {}
inline void socketGlobalShutdown() {}

}  // namespace theseed::runtime::detail

#endif
