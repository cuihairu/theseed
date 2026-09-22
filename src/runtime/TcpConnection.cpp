#include "theseed/runtime/TcpConnection.h"

#include "SocketDetail.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>

namespace theseed::runtime {

namespace {

detail::SocketHandle toSocket(std::uintptr_t h) {
    return static_cast<detail::SocketHandle>(h);
}

std::uintptr_t fromSocket(detail::SocketHandle s) {
    return static_cast<std::uintptr_t>(s);
}

}  // namespace

TcpConnection::~TcpConnection() {
    close();
}

void TcpConnection::globalInit() {
    detail::socketGlobalInit();
}

void TcpConnection::globalShutdown() {
    detail::socketGlobalShutdown();
}

void TcpConnection::setNonBlocking() {
    detail::setNonBlocking(toSocket(socket_));
}

bool TcpConnection::connect(const std::string& host, std::uint16_t port) {
    if (connected_) return false;

    detail::SocketHandle s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == detail::kInvalidSocket) return false;

    socket_ = fromSocket(s);
    setNonBlocking();

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    auto result = ::connect(s,
                            reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (result == detail::kSocketError && !detail::connectInProgress()) {
        detail::closeSocket(s);
        socket_ = 0;
        return false;
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
        detail::closeSocket(toSocket(socket_));
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
        auto n = ::recv(toSocket(socket_), buf, sizeof(buf), 0);
        if (n == detail::kSocketError) {
            if (!detail::wouldBlock()) {
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

    auto n = ::send(toSocket(socket_),
                    reinterpret_cast<const char*>(sendBuffer_.data()),
                    static_cast<int>(sendBuffer_.size()), detail::kSendFlags);
    if (n == detail::kSocketError) {
        if (detail::wouldBlock()) {
            return false;
        }
        connected_ = false;
        return false;
    }

    sendBuffer_.erase(sendBuffer_.begin(), sendBuffer_.begin() + n);
    return sendBuffer_.empty();
}

}  // namespace theseed::runtime
