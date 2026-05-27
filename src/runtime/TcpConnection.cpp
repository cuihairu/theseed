#include "theseed/runtime/TcpConnection.h"

#ifndef _WINSOCK2API_
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <cstring>
#include <stdexcept>

namespace theseed::runtime {

namespace {

SOCKET toSocket(std::uintptr_t h) {
    return static_cast<SOCKET>(h);
}

std::uintptr_t fromSocket(SOCKET s) {
    return static_cast<std::uintptr_t>(s);
}

}  // namespace

TcpConnection::~TcpConnection() {
    close();
}

void TcpConnection::globalInit() {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
}

void TcpConnection::globalShutdown() {
    WSACleanup();
}

void TcpConnection::setNonBlocking() {
    auto s = toSocket(socket_);
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
}

bool TcpConnection::connect(const std::string& host, std::uint16_t port) {
    if (connected_) return false;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;

    socket_ = fromSocket(s);
    setNonBlocking();

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    auto result = ::connect(s,
                            reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (result == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            closesocket(s);
            socket_ = 0;
            return false;
        }
    }

    connected_ = true;
    return true;
}

bool TcpConnection::write(std::span<const std::byte> data) {
    if (!connected_ || data.empty()) return connected_;

    sendBuffer_.insert(sendBuffer_.end(), data.begin(), data.end());
    trySendBuffered();
    return true;
}

void TcpConnection::setOnReceived(std::function<void(std::span<const std::byte>)> callback) {
    onReceived_ = std::move(callback);
}

void TcpConnection::close() {
    if (socket_ != 0) {
        closesocket(toSocket(socket_));
    }
    socket_ = 0;
    connected_ = false;
    sendBuffer_.clear();
}

bool TcpConnection::isConnected() const {
    return connected_;
}

void TcpConnection::pump() {
    static_cast<void>(pumpWithResult());
}

std::size_t TcpConnection::pumpWithResult() {
    if (!connected_) return 0;

    std::size_t totalReceived = 0;

    // Read available data
    char buf[4096];
    while (true) {
        auto n = recv(toSocket(socket_), buf, sizeof(buf), 0);
        if (n == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK) {
                connected_ = false;
            }
            break;
        }
        if (n == 0) {
            connected_ = false;
            break;
        }

        totalReceived += static_cast<std::size_t>(n);
        if (onReceived_) {
            onReceived_(std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(buf),
                static_cast<std::size_t>(n)));
        }
    }

    // Flush pending sends
    trySendBuffered();

    return totalReceived;
}

bool TcpConnection::trySendBuffered() {
    if (sendBuffer_.empty()) return true;

    auto n = send(toSocket(socket_),
                  reinterpret_cast<const char*>(sendBuffer_.data()),
                  static_cast<int>(sendBuffer_.size()), 0);
    if (n == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            return false;
        }
        connected_ = false;
        return false;
    }

    sendBuffer_.erase(sendBuffer_.begin(), sendBuffer_.begin() + n);
    return sendBuffer_.empty();
}

}  // namespace theseed::runtime
