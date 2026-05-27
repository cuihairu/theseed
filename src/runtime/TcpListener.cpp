#include "theseed/runtime/TcpListener.h"
#include "theseed/runtime/TcpConnection.h"

#ifndef _WINSOCK2API_
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace theseed::runtime {

namespace {

SOCKET toSocket(std::uintptr_t h) {
    return static_cast<SOCKET>(h);
}

}  // namespace

TcpListener::TcpListener() = default;

TcpListener::~TcpListener() {
    if (socket_ != 0) {
        closesocket(toSocket(socket_));
        socket_ = 0;
    }
}

bool TcpListener::listen(const std::string& host, std::uint16_t port, int backlog) {
    if (listening_) return false;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;

    // Allow address reuse
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return false;
    }

    if (::listen(s, backlog) == SOCKET_ERROR) {
        closesocket(s);
        return false;
    }

    // Non-blocking so accept() doesn't hang the tick loop
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);

    // Get actual bound port (for port=0 ephemeral)
    int addrLen = sizeof(addr);
    getsockname(s, reinterpret_cast<sockaddr*>(&addr), &addrLen);
    localPort_ = ntohs(addr.sin_port);

    socket_ = static_cast<std::uintptr_t>(s);
    listening_ = true;
    return true;
}

void TcpListener::close() {
    if (socket_ != 0) {
        closesocket(toSocket(socket_));
    }
    socket_ = 0;
    listening_ = false;
}

void TcpListener::setConnectionFactory(ConnectionFactory factory) {
    factory_ = std::move(factory);
}

std::shared_ptr<TcpConnection> TcpListener::accept() {
    if (!listening_) return nullptr;

    sockaddr_in clientAddr{};
    int clientLen = sizeof(clientAddr);
    SOCKET clientSocket = ::accept(
        toSocket(socket_),
        reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);

    if (clientSocket == INVALID_SOCKET) return nullptr;

    auto conn = factory_ ? factory_() : TcpConnection::create();
    conn->socket_ = static_cast<std::uintptr_t>(clientSocket);
    conn->connected_ = true;
    conn->setNonBlocking();
    return conn;
}

bool TcpListener::isListening() const {
    return listening_;
}

std::uint16_t TcpListener::localPort() const {
    return localPort_;
}

}  // namespace theseed::runtime
