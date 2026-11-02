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
    // 发送一帧。对端滞止（TcpConnection 待发缓冲超水位）或连接失效时
    // write() 返回 false：本方法随即关闭管道并返回 false，交由上层清理，
    // 避免静默丢包（审核发现）。返回 true 表示写入成功。
    bool send(std::span<const std::byte> data);
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
