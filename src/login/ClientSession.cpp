#include "theseed/login/ClientSession.h"
#include "theseed/login/LoginProtocol.h"

#include <cstring>

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

void ClientSession::send(std::span<const std::byte> data) {
    if (pipe_ && pipe_->isConnected()) {
        pipe_->write(data);
    }
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
    if (data.empty()) return;
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
