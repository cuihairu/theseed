#pragma once

#include "theseed/runtime/IBytePipe.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace theseed::runtime {

class InMemoryBytePipe;

class TcpConnection final : public IBytePipe {
public:
    static std::shared_ptr<TcpConnection> create() {
        return std::shared_ptr<TcpConnection>(new TcpConnection());
    }

    ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    bool connect(const std::string& host, std::uint16_t port);
    bool write(std::span<const std::byte> data) override;
    void setOnReceived(std::function<void(std::span<const std::byte>)> callback) override;
    void close() override;
    bool isConnected() const override;

    void pump() override;
    std::size_t pumpWithResult();

    static void globalInit();
    static void globalShutdown();

private:
    TcpConnection() = default;

    void setNonBlocking();
    bool trySendBuffered();

    std::uintptr_t socket_ = 0;
    bool connected_ = false;
    std::function<void(std::span<const std::byte>)> onReceived_;
    std::vector<std::byte> sendBuffer_;
    std::vector<std::byte> recvBuffer_;

    friend class TcpListener;
};

}  // namespace theseed::runtime
