#include "theseed/runtime/TcpListener.h"
#include "theseed/runtime/TcpConnection.h"

#include "SocketDetail.h"

namespace theseed::runtime {

namespace {

detail::SocketHandle toSocket(std::uintptr_t h) {
    return static_cast<detail::SocketHandle>(h);
}

}  // namespace

TcpListener::TcpListener() = default;

TcpListener::~TcpListener() {
    if (socket_ != 0) {
        detail::closeSocket(toSocket(socket_));
        socket_ = 0;
    }
}

bool TcpListener::listen(const std::string& host, std::uint16_t port, int backlog) {
    if (listening_) return false;

    detail::SocketHandle s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == detail::kInvalidSocket) return false;

    // Allow address reuse
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == detail::kSocketError) {
        detail::closeSocket(s);
        return false;
    }

    if (::listen(s, backlog) == detail::kSocketError) {
        detail::closeSocket(s);
        return false;
    }

    // Non-blocking so accept() doesn't hang the tick loop
    detail::setNonBlocking(s);

    // Get actual bound port (for port=0 ephemeral)
    detail::SockLen addrLen = sizeof(addr);
    getsockname(s, reinterpret_cast<sockaddr*>(&addr), &addrLen);
    localPort_ = ntohs(addr.sin_port);

    socket_ = static_cast<std::uintptr_t>(s);
    listening_ = true;
    return true;
}

void TcpListener::close() {
    if (socket_ != 0) {
        detail::closeSocket(toSocket(socket_));
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
    detail::SockLen clientLen = sizeof(clientAddr);
    detail::SocketHandle clientSocket = ::accept(
        toSocket(socket_),
        reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);

    if (clientSocket == detail::kInvalidSocket) return nullptr;

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
