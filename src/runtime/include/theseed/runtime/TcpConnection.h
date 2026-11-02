#pragma once

#include "theseed/runtime/IBytePipe.h"

#include <cstddef>
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
        return std::shared_ptr<TcpConnection>(new TcpConnection());  // LCOV_EXCL_BR_LINE shared_ptr 控制块构造内联分支归因 return 行，伪影
    }

    ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    bool connect(const std::string& host, std::uint16_t port);
    bool write(std::span<const std::byte> data) override;
    void setOnReceived(std::function<void(std::span<const std::byte>)> callback) override;
    void close() override;
    bool isConnected() const override;
    // 待发缓冲上限（字节）：对端滞止时 write 超过该水位即拒绝并返回 false，
    // 防止无界增长（P2-1 深审）。0 = 不设上限（测试/回环兼容）。
    void setMaxSendBuffer(std::size_t maxBytes);
    std::size_t maxSendBuffer() const noexcept;
    // 当前待发缓冲字节数（诊断/测试）。
    std::size_t sendBufferSize() const noexcept;

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
    // 非阻塞 connect 已发起但 SO_ERROR 尚未落定；pump 首轮裁决。
    bool connectPending_ = false;
    std::function<void(std::span<const std::byte>)> onReceived_;
    std::vector<std::byte> sendBuffer_;
    std::vector<std::byte> recvBuffer_;
    std::size_t maxSendBuffer_ = 8 * 1024 * 1024;

    friend class TcpListener;
};

}  // namespace theseed::runtime
