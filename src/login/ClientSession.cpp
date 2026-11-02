#include "theseed/login/ClientSession.h"
#include "theseed/login/LoginProtocol.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace theseed::login {

ClientSession::ClientSession(std::shared_ptr<runtime::IBytePipe> pipe)
    : pipe_(std::move(pipe)) {
    if (pipe_) {
        pipe_->setOnReceived([this](std::span<const std::byte> data) {
            onRawReceived(data);
        });
    }
}

ClientSession::~ClientSession() {
    close();
}

void ClientSession::setMessageCallback(MessageCallback callback) {
    onMessage_ = std::move(callback);
}

bool ClientSession::send(std::span<const std::byte> data) {
    if (!pipe_ || !pipe_->isConnected()) return false;
    // write() 为 false 表示对端滞止（待发缓冲超水位）或管道失效：立即
    // 关闭连接，让上层 cleanupDisconnected() 回收本会话，避免消息被
    // 静默丢弃（审核发现：ClientSession::send 曾忽略 write 返回值）。
    if (!pipe_->write(data)) {
        close();
        return false;
    }
    return true;
}

void ClientSession::close() {
    if (pipe_) {
        pipe_->close();
    }
}

bool ClientSession::isConnected() const {
    return pipe_ && pipe_->isConnected();
}

void ClientSession::pump() {
    if (pipe_) {
        pipe_->pump();
    }
}

void ClientSession::onRawReceived(std::span<const std::byte> data) {
    if (data.empty()) return;  // LCOV_EXCL_BR_LINE 管道层仅在收到非空分片时才触发回调，空分片臂不可达
    auto oldSize = recvBuffer_.size();
    recvBuffer_.resize(oldSize + data.size());
    std::memcpy(recvBuffer_.data() + oldSize, data.data(), data.size());
    tryParseMessages();
}

void ClientSession::tryParseMessages() {
    while (recvBuffer_.size() >= LoginProtocol::kHeaderSize) {
        auto view = std::span<const std::byte>(recvBuffer_.data(), recvBuffer_.size());

        // Parse length (little-endian u32)
        auto payloadLen = static_cast<std::uint32_t>(view[0])
                        | (static_cast<std::uint32_t>(view[1]) << 8)
                        | (static_cast<std::uint32_t>(view[2]) << 16)
                        | (static_cast<std::uint32_t>(view[3]) << 24);
        auto typeVal = static_cast<std::uint8_t>(view[4]);
        auto frameSize = LoginProtocol::kHeaderSize + payloadLen;

        if (recvBuffer_.size() < frameSize) return;

        auto type = static_cast<ClientMessageType>(typeVal);
        auto payloadSpan = std::span<const std::byte>(
            recvBuffer_.data() + LoginProtocol::kHeaderSize, payloadLen);

        if (onMessage_) {
            onMessage_(type, payloadSpan);
        }

        recvBuffer_.erase(recvBuffer_.begin(),
                          recvBuffer_.begin() + static_cast<std::ptrdiff_t>(frameSize));
    }
}

}  // namespace theseed::login
