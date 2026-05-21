# Communication — 跨进程通信与传输

> EntityCall 是 theseed 进程间通信的核心抽象，Transport 负责底层消息传输。
>
> 来源：BigWorld Mailbox + Mercury，KBEngine EntityCall + TCP。
> 当前实现基线以 [0-foundation/01-mvp-architecture-baseline](../0-foundation/01-mvp-architecture-baseline.md) 为准。

---

## 1. EntityCall

### 1.1 EntityCall 是什么

```
EntityCall 不是"远程方法调用框架"。
它是"实体在另一个进程上的代理句柄"。

entity.base.onLevelUp(35)
  │       │
  │       └── base 是一个 EntityCall，指向 Base 侧实体
  │
  └── 在 Cell 侧执行，通过 EntityCall 把调用发到 BaseApp

本质：
  EntityCall = (EntityID, ComponentID, EntityType)
  调用 EntityCall 的方法 = 发送一条消息到目标进程

关键区别：
  EntityCall 是有状态的——它绑定到特定实体
  gRPC 是无状态的——它只绑定到服务端点
```

### 1.2 核心设计决策：EntityCall 必须走 Runtime Data Plane

```
重要：EntityCall 不走 MessageBus！

原因：
  1. 顺序保证
     EntityCall 属于实体权威路径
     同一实体的消息必须具备明确顺序语义

  2. 延迟要求
     EntityCall 处于 tick 内关键路径
     不应经过额外 broker 跳数

  3. 路由敏感
     Runtime 需要知道实体当前在哪个进程
     这个路由信息属于引擎内部状态，不属于 MQ 主题路由

  4. 背压控制
     tick 内不能无限发消息
     Runtime Data Plane 需要有可观测、可控制的发送水位

结论：
  EntityCall / Ghost 同步 / 迁移数据
    → Runtime Data Plane

  控制面消息
    → MessageBus / Control Plane

  跨服异步与桥接
    → Cross-Realm Async Plane
```

### 1.3 EntityCall 接口

```cpp
// runtime/EntityCall.h

class EntityCall {
public:
    EntityCall(EntityId id, ComponentId target, const std::string& entityType);

    // 调用远端方法（fire-and-forget）
    void call(const std::string& method, const Args& args);

    // 调用远端方法（带回调）
    void callWithCallback(const std::string& method,
                          const Args& args,
                          CallbackPtr callback);

    // 底层：构造 Bundle 并发送
    void sendCall(Bundle* bundle);

    // 路由：决定消息发到哪个进程
    ComponentId targetComponent() const;
    void updateTarget(ComponentId newTarget);  // 迁移后更新路由

    // 状态
    bool isValid() const;
    const Address& address() const;

private:
    EntityId entityId_;
    ComponentId targetComponent_;
    uint16_t entityTypeUType_;
    Address targetAddr_;
    Channel* channel_;
};
```

### 1.4 Base ↔ Cell 通信

```
通信模式：

Base → Cell:
  Base 持有 cellEntityCall
  通过 cellEntityCall.call() 发消息到 CellApp

Cell → Base:
  Cell 持有 baseEntityCall
  通过 baseEntityCall.call() 发消息到 BaseApp

创建流程：
  1. Base 创建实体 → DB 写入
  2. Base 通过 EntityCall 请求 CellApp 创建 Cell Entity
  3. CellApp 创建成功后，Cell Entity 持有 baseEntityCall
  4. 双向通信链路建立

销毁流程：
  1. Cell 调用 destroy() → 通知 Base onLoseCell
  2. Base 决定是否销毁 Base Entity
  3. 如果是，Base 销毁 → 通知 DB 删除
  4. 如果不是（如玩家断线但保留数据），Base 保持存活
```

---

## 2. Runtime Delivery Classes — MVP 传输语义

> 来源：BigWorld Mercury 的分级思想 + KBEngine 的简单路径。
> 但 MVP 不直接承诺“四级都已实现”，只定义当前可落地的两类运行时语义。
> BigWorld 原始的 `driver / passenger / critical / none` 语义与 piggyback / overflow / inactivity 边界，见 [../3-infrastructure/07-runtime-transport-reliability](../3-infrastructure/07-runtime-transport-reliability.md)。

### 2.1 为什么需要分级

```
游戏服务器同一条连接上的消息，可靠性需求截然不同：

必须可靠（丢了就崩）：
  - EntityCall 方法调用
  - 实体创建 / 销毁
  - 迁移序列化数据
  - Base ↔ Cell 控制消息

丢了无所谓（下帧覆盖）：
  - 位置更新（每 tick 都有新位置）
  - 朝向更新
  - 速度同步

可重算：
  - 视野辅助通知
  - 过期状态刷新
```

### 2.2 MVP 两类语义

```cpp
enum class DeliveryClass : uint8_t {
    ORDERED_RELIABLE = 0,   // 保序 + 可靠送达
    UNORDERED_LOSSY  = 1,   // 可合并、可丢弃、被新状态覆盖
};
```

```
ORDERED_RELIABLE 用于：
  - EntityCall
  - 实体创建 / 销毁
  - Base ↔ Cell 控制消息
  - 迁移数据
  - Ghost 核心状态同步

UNORDERED_LOSSY 用于：
  - 位置 / 朝向等 volatile 更新
  - 可重算的视野辅助事件
  - 过期即无意义的状态刷新
```

```
远期可以演进到 BigWorld 风格的更细粒度分级，
但前提是 Runtime 先具备明确的确认、重放、排队和监控机制。
在此之前，文档不把四级抽象写成既成实现。
```

```
也就是说：
  当前 theseed 文档覆盖的是“业务可见的最小送达语义”
  不是 BigWorld Mercury 在 Bundle / Channel 层的全部可靠性角色
```

### 2.3 为什么推荐 Aeron

```
三种方案对比：

┌──────────┬──────────────┬──────────────┬──────────────┐
│          │ Mercury      │ KCP          │ Aeron        │
│          │ (BigWorld)   │              │ (theseed 选) │
├──────────┼──────────────┼──────────────┼──────────────┤
│ 传输层   │ UDP + 自建    │ UDP + 自建   │ UDP / IPC     │
│ 零拷贝   │ 否           │ 否           │ 是            │
│ 背压     │ 无           │ 无           │ 有            │
│ 多播     │ 无           │ 无           │ 有            │
│ 同机 IPC │ 无           │ 无           │ 有            │
│ 分片重组 │ 手动         │ 手动         │ 内建          │
│ 延迟     │ 极低         │ 低           │ 极低          │
│ C++ SDK  │ 无           │ C            │ C++（官方）   │
└──────────┴──────────────┴──────────────┴──────────────┘

theseed 推荐 Aeron 的理由：

  1. 作为 Runtime Data Plane 的实现基础很合适
     - 官方支持高性能 UDP 单播、组播和 IPC
     - 同机 IPC 路径对 BaseApp / CellApp 很友好

  2. 零拷贝
     - memory-mapped buffer（AtomicBuffer）
     - 同机 IPC 延迟很低

  3. 背压能力
     - Publication.offer() 能暴露发送压力
     - 适合 tick 模型做丢弃、合并和重试策略

  4. 多播和旁路订阅扩展性好

  5. 成熟的 C++ SDK

  6. theseed 的 "lossy" 语义由 Runtime 决定
     - 过期的 volatile 更新可以在发送前合并或丢弃
     - 不要求把所有语义都下沉为底层协议开关
```

### 2.4 Runtime Transport 架构

```
┌─────────────────────────────────────────────────────────┐
│ theseed Runtime                                         │
│                                                          │
│  IRuntimeTransport（theseed 抽象接口）                   │
│       │                                                  │
│       ├── AeronTransport（推荐，内网进程间）              │
│       │     ├── Ordered Stream: EntityCall / 迁移        │
│       │     ├── State Delta Stream: Ghost / Witness      │
│       │     ├── Volatile Stream: 位置 / 朝向             │
│       │     └── 同机 IPC: aeron:ipc（共享内存）          │
│       │                                                  │
│       └── TCPTransport（备选，简单部署）                 │
└─────────────────────────────────────────────────────────┘

建议的 Stream 划分：

  Stream ID    用途                            DeliveryClass
  ──────────   ───────────────────────────     ────────────────
  1001         EntityCall                      ORDERED_RELIABLE
  1002         状态同步（real→ghost / witness） ORDERED_RELIABLE
  1003         位置/朝向 volatile 更新          UNORDERED_LOSSY
  1004         实体创建/销毁                    ORDERED_RELIABLE
  1005         迁移数据                        ORDERED_RELIABLE

说明：
  - 控制面消息不在 Runtime Transport 文档中定义
  - MessageBus 自己维护 subject / queue / request-reply 语义
```

### 2.5 核心接口

```cpp
// runtime/RuntimeTransport.h

class IRuntimeTransport {
public:
    virtual ~IRuntimeTransport() = default;

    virtual void send(const Address& target,
                      const Message& msg,
                      DeliveryClass deliveryClass) = 0;

    virtual void registerHandler(MessageId id,
                                 MessageHandler handler) = 0;

    virtual Channel* getChannel(const Address& addr) = 0;
    virtual void closeChannel(const Address& addr) = 0;

    virtual void tick() = 0;

    virtual TransportStats getStats() const = 0;
};
```

### 2.6 Aeron 使用示例

```cpp
// runtime/AeronTransport.h

class AeronTransport : public IRuntimeTransport {
public:
    void init(const AeronConfig& config) {
        aeron::Context ctx;
        ctx.errorHandler([](const std::exception& ex) {
            LOG_ERROR("aeron error", {{"what", ex.what()}});
        });

        aeron_ = aeron::Aeron::connect(ctx);

        const std::string channel = config.useIPC
            ? "aeron:ipc"
            : "aeron:udp?endpoint=" + config.endpoint;

        orderedPub_ = getPublication(channel, STREAM_ORDERED);
        stateDeltaPub_ = getPublication(channel, STREAM_STATE_DELTA);
        volatilePub_ = getPublication(channel, STREAM_VOLATILE);

        orderedSub_ = getSubscription(channel, STREAM_ORDERED);
        stateDeltaSub_ = getSubscription(channel, STREAM_STATE_DELTA);
        volatileSub_ = getSubscription(channel, STREAM_VOLATILE);
    }

    void send(const Address& target,
              const Message& msg,
              DeliveryClass deliveryClass) override {
        auto& pub =
            deliveryClass == DeliveryClass::ORDERED_RELIABLE
                ? *orderedPub_
                : *volatilePub_;

        aeron::concurrent::AtomicBuffer buffer(msg.data(), msg.size());

        std::int64_t result;
        do {
            result = pub.offer(buffer, 0, msg.size());
            if (result == aeron::Publication::BACK_PRESSURED) {
                METRIC_COUNTER("transport.backpressure", 1);

                if (deliveryClass == DeliveryClass::ORDERED_RELIABLE) {
                    enqueueRetry(target, msg);
                } else {
                    coalesceVolatile(target, msg);
                }
                return;
            }
        } while (result < 0 && result != aeron::Publication::NOT_CONNECTED);
    }

    void tick() override {
        orderedSub_->poll(messageHandler_, 100);
        stateDeltaSub_->poll(messageHandler_, 200);
        volatileSub_->poll(messageHandler_, 200);
    }

private:
    std::shared_ptr<aeron::Aeron> aeron_;
    std::shared_ptr<aeron::Publication> orderedPub_;
    std::shared_ptr<aeron::Publication> stateDeltaPub_;
    std::shared_ptr<aeron::Publication> volatilePub_;
    std::shared_ptr<aeron::Subscription> orderedSub_;
    std::shared_ptr<aeron::Subscription> stateDeltaSub_;
    std::shared_ptr<aeron::Subscription> volatileSub_;
    aeron::FragmentAssembler assembler_;
};
```

### 2.7 消息头格式

```
Bundle 消息头格式（theseed）：

┌─────────────────────────────────────────┐
│ Message ID (2 bytes)                    │
│ DeliveryClass (1 bit)                   │
│ Compressed (1 bit)                      │
│ Reserved (6 bits)                       │
│ Payload Length (2 bytes)                │
├─────────────────────────────────────────┤
│ Payload ...                             │
└─────────────────────────────────────────┘

发送端：
  1. 根据消息类型选择 DeliveryClass
  2. ORDERED_RELIABLE 消息进入保序发送队列
  3. UNORDERED_LOSSY 消息允许被合并或丢弃

接收端：
  1. ORDERED_RELIABLE 路径按流内顺序处理
  2. UNORDERED_LOSSY 路径按最新状态覆盖旧状态
```

### 2.8 与两套引擎的对比

| 维度 | BigWorld Mercury | KBEngine TCP | theseed Aeron |
|------|-----------------|-------------|---------------|
| 内部协议 | UDP + 自建可靠性 | TCP | UDP + Aeron Media Driver |
| 运行时语义 | 4 级（同一通道混用） | 无（全可靠） | MVP 先实现 2 类语义 |
| 零拷贝 | 否 | 否 | 是 |
| 背压 | 无 | TCP 内建 | Publication.offer() 流控 |
| 多播 | 无 | 无 | UDP multicast |
| 同机优化 | UDP loopback | TCP loopback | aeron:ipc 共享内存 |
| 延迟 | 极低 | 较高 | 极低（同机 < 1μs） |
