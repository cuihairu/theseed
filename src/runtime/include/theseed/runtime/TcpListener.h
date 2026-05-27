#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace theseed::runtime {

class TcpConnection;

class TcpListener final {
public:
    using ConnectionFactory = std::function<std::shared_ptr<TcpConnection>()>;

    TcpListener();
    ~TcpListener();

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    bool listen(const std::string& host, std::uint16_t port, int backlog = 16);
    void close();

    void setConnectionFactory(ConnectionFactory factory);

    std::shared_ptr<TcpConnection> accept();
    bool isListening() const;

    std::uint16_t localPort() const;

private:
    std::uintptr_t socket_ = 0;
    ConnectionFactory factory_;
    bool listening_ = false;
    std::uint16_t localPort_ = 0;
};

}  // namespace theseed::runtime
