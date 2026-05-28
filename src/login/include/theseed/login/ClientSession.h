#pragma once

#include "theseed/login/LoginProtocol.h"
#include "theseed/runtime/IBytePipe.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <span>

namespace theseed::login {

class ClientSession {
public:
    using MessageCallback = std::function<void(ClientMessageType, std::span<const std::byte>)>;

    explicit ClientSession(std::shared_ptr<runtime::IBytePipe> pipe);
    ~ClientSession();

    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    void setMessageCallback(MessageCallback callback);
    void send(std::span<const std::byte> data);
    void close();
    bool isConnected() const;
    void pump();

private:
    void onRawReceived(std::span<const std::byte> data);
    void tryParseMessages();

    std::shared_ptr<runtime::IBytePipe> pipe_;
    MessageCallback onMessage_;
    std::vector<std::byte> recvBuffer_;
};

}  // namespace theseed::login
