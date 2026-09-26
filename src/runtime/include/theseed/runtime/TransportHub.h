#pragma once

#include "theseed/runtime/RuntimeTransport.h"

#include <cstddef>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace theseed::runtime {

class TransportHub final : public IRuntimeTransport {
public:
    explicit TransportHub(ComponentId localComponent);

    TransportHub(const TransportHub&) = delete;
    TransportHub& operator=(const TransportHub&) = delete;

    void connectPeer(ComponentId peerId, std::shared_ptr<IRuntimeTransport> transport);
    // 服务端入站连接：对端身份（sourceComponent）事先未知，由其首条入站
    // 请求自报注册。DBApp 这类被动监听的服务用它替代 connectPeer——
    // 否则回复按请求 sourceComponent 路由时找不到 peer，静默丢包。
    void attachServerTransport(std::shared_ptr<IRuntimeTransport> transport);
    void disconnectPeer(ComponentId peerId);
    bool hasPeer(ComponentId peerId) const;
    std::size_t peerCount() const;

    // IRuntimeTransport
    SendResult send(RuntimeInvocation invocation) override;
    std::size_t receive(ComponentId targetComponent,
                        RuntimeInvocation* out,
                        std::size_t capacity) override;
    std::size_t pendingCount() const override;
    void flush() override;
    TransportStats stats() const override;

    void tick() override;

private:
    mutable std::mutex mutex_;
    std::unordered_map<ComponentId, std::shared_ptr<IRuntimeTransport>> peers_;
    // 服务端连接：等待首条入站消息完成身份注册，注册后从列表移除。
    std::vector<std::shared_ptr<IRuntimeTransport>> awaitingIdentity_;
};

}  // namespace theseed::runtime
