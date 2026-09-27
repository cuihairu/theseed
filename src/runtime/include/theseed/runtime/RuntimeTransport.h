#pragma once

#include "theseed/runtime/RuntimeTypes.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace theseed::runtime {

struct RuntimeInvocation final {
    EntityId entityId = 0;
    ComponentId sourceComponent = 0;
    ComponentId targetComponent = 0;
    // §6.2 审计关联 id：由发起方（运维控制台）铸造，逐请求唯一；0 =
    // 请求未携带（进程内默认调用方不铸 id）。跨组件透传，接收侧入审计
    // 条目用于请求↔动作对账。无版本协商——同仓同版本对端共同演进。
    std::uint64_t requestId = 0;
    std::string entityType;
    std::string method;
    DeliveryClass deliveryClass = DeliveryClass::ORDERED_RELIABLE;
    std::vector<std::byte> payload;
};

class IRuntimeTransport {
public:
    virtual ~IRuntimeTransport() = default;  // LCOV_EXCL_LINE C++ ABI：trivial 虚析构是空体，gcc 不为其生成计数指令，D0/D1/D2 三符号变体恒 0（结构不可测）

    virtual SendResult send(RuntimeInvocation invocation) = 0;
    virtual std::size_t receive(ComponentId targetComponent,
                                RuntimeInvocation* out,
                                std::size_t capacity) = 0;
    virtual std::size_t pendingCount() const = 0;
    virtual void flush() = 0;
    virtual TransportStats stats() const = 0;

    // 链路活性查询。缺省为真：内存 transport 没有"断开"概念。TCP 实现
    // 按 socket 实况回答（对端关闭/连接失败即假），供上层做断链监督与
    // 重连决策（如 LoginApp 的通知腿）——hub 只持接口，活性判定必须走
    // 这里而不是 downcast。
    virtual bool isConnected() const { return true; }

    // 周期驱动：读管道入站、冲刷出站。TCP 子类在 tick 里 pump socket，
    // 内存实现为空操作。TransportHub::tick 借此驱动所有 peer 的入站——
    // 缺了这一环，服务端永远读不到 socket 上的请求。
    virtual void tick();
};

class InMemoryRuntimeTransport final : public IRuntimeTransport {
public:
    InMemoryRuntimeTransport() = default;

    InMemoryRuntimeTransport(const InMemoryRuntimeTransport&) = delete;
    InMemoryRuntimeTransport& operator=(const InMemoryRuntimeTransport&) = delete;

    SendResult send(RuntimeInvocation invocation) override;
    std::size_t receive(ComponentId targetComponent,
                        RuntimeInvocation* out,
                        std::size_t capacity) override;
    std::size_t pendingCount() const override;
    void flush() override;
    TransportStats stats() const override;
    void tick() override;

    std::size_t drain(RuntimeInvocation* out, std::size_t capacity);

private:
    mutable std::mutex mutex_;
    std::deque<RuntimeInvocation> invocations_;
    TransportStats stats_;
};

// §6.2 关联 id 铸造（发起方边界用）：进程内单调递增、逐请求唯一；0 恒
// 不返回（0 = 未携带的哨兵值，见 RuntimeInvocation::requestId）。唯一性
// 口径沿用规格注记「逐请求唯一是发起方责任」——跨进程/跨重启唯一性由
// 部署方保证（当前唯一生产发起方是单进程组件：LoginApp 注册探针与
// 中心本地动作；审计环形容量有界，进程内唯一已覆盖对账窗口）。
// 注：置于全部类定义之后——插在类间会移动 IRuntimeTransport 析构克隆
// 的 gcov 行归属点（不同 TU 记到不同行），触发 gcovr 严格合并断言
//（CI 覆盖率腿同雷）。
inline std::uint64_t mintRequestId() {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace theseed::runtime
