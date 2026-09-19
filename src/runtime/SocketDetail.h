#pragma once

// 平台 socket 细节：Winsock2 与 POSIX sockets 的差异集中在此，
// TcpConnection / TcpListener 只面向统一接口编码。

#if defined(_WIN32)

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
inline void closeSocket(SocketHandle s) { closesocket(s); }

inline void socketGlobalInit() {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
}
inline void socketGlobalShutdown() { WSACleanup(); }

}  // namespace theseed::runtime::detail

#else

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
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
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

// 非阻塞 connect 尚在进行中（POSIX 以 EINPROGRESS 表达）。
inline bool connectInProgress() {
    return errno == EINPROGRESS || errno == EINTR || errno == EAGAIN ||
           errno == EWOULDBLOCK;
}

inline void setNonBlocking(SocketHandle s) {
    const int flags = ::fcntl(s, F_GETFL, 0);
    ::fcntl(s, F_SETFL, flags | O_NONBLOCK);
}
inline void closeSocket(SocketHandle s) { ::close(s); }

inline void socketGlobalInit() {}
inline void socketGlobalShutdown() {}

}  // namespace theseed::runtime::detail

#endif
