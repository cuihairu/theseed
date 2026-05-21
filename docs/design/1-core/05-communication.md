# Communication — 跨进程通信与传输

> EntityCall 是 theseed 进程间通信的核心抽象，Transport 负责底层消息可靠传输。
>
> 来源：BigWorld Mailbox + Mercury，KBEngine EntityCall + TCP。
> theseed 选择 Aeron 作为传输层，融合 BigWorld 的可靠性分级思想。

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

### 1.2 核心设计决策：EntityCall 必须走 Runtime Transport

```
重要：EntityCall 不走 MessageBus！

原因：
  1. 顺序保证：同一 Channel 上的消息必须保序
     MessageBus（NATS）不保证消息顺序
     Runtime Transport（TCP 直连）保证 FIFO

  2. 延迟：EntityCall 是 tick 内同步操作
     NATS 中间层增加至少 1-2ms 延迟
     TCP 直连是 sub-ms

  3. 所有权敏感：EntityCall 涉及实体权威
     消息路由需要知道实体当前在哪个进程
     这个路由信息在 Runtime 内部维护，不在 MQ 中

  4. 背压：tick 内不能无限发消息
     Runtime Transport 有 Channel 水位控制
     MQ 通常不做 tick 级背压

结论：
  EntityCall → Runtime Transport（进程间 TCP 直连）
  控制面消息 → MessageBus（NATS）
  异步任务 → MessageBus
  跨服桥接 → MessageBus
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

## 2. Transport Reliability — 消息可靠性分级

> 来源：BigWorld Mercury UDP 四级可靠性。
> KBEngine 选 TCP，简单但一刀切。theseed 取两者之长：同一连接上支持可靠性分级。

### 2.1 为什么需要分级

```
游戏服务器同一条连接上的消息，可靠性需求截然不同：

必须可靠（丢了就崩）：
  - EntityCall 方法调用
  - 实体创建 / 销毁
  - 属性持久化写入
  - 迁移序列化数据

丢了无所谓（下帧覆盖）：
  - 位置更新（每 tick 都有新位置）
  - 朝向更新
  - 速度同步

尽量可靠但不致命：
  - 属性同步（可以容忍偶尔丢一帧，下帧补上）
  - AOI 进入/离开通知（丢了会在下个 tick 重算）
```

### 2.2 四级可靠性（来自 BigWorld Mercury）

```
theseed 的可靠性分级：

enum class Reliability : uint8_t {
    NONE        = 0,   // 不保证送达，不重传（位置/朝向）
    LOW         = 1,   // 尽力送达，丢了不重传（属性同步）
    HIGH        = 2,   // 保证送达，重传直到 ACK（EntityCall）
    CRITICAL    = 3,   // 保证送达 + 保证顺序（实体创建/迁移/销毁）
};
```

### 2.3 Aeron 作为传输层

```
三种方案对比：

┌──────────┬──────────────┬──────────────┬──────────────┐
│          │ Mercury      │ KCP          │ Aeron        │
│          │ (BigWorld)   │              │ (theseed 选) │
├──────────┼──────────────┼──────────────┼──────────────┤
│ 传输层   │ UDP + 自建    │ UDP + 自建   │ UDP + 媒体驱动│
│ 可靠/不可靠│ 同通道混用    │ 同通道混用   │ 同通道混用    │
│ 零拷贝   │ 否           │ 否           │ 是（memory-mapped）│
│ 背压     │ 无           │ 无           │ 有（publisher flow control）│
│ 多播     │ 无           │ 无           │ 有（UDP multicast）│
│ 同机 IPC  │ 无           │ 无           │ 有（aeron:ipc shared memory）│
│ 分片重组  │ 手动         │ 手动         │ 内建（fragment assembler）│
│ 延迟     │ 极低         │ 低           │ 极低（同机 IPC sub-μs）│
│ C++ SDK  │ 无           │ C            │ C++（官方）   │
│ 成熟度   │ BigWorld 内部 │ 游戏行业广泛  │ 金融/HFT 验证  │
└──────────┴──────────────┴──────────────┴──────────────┘

theseed 选择 Aeron 的理由：

  1. Mercury 级能力，但不用自己写可靠性层
     - Aeron 内建 reliable=true/false：同一条连接混用
     - 等价于 Mercury 的 RELIABLE_NO / RELIABLE_CRITICAL

  2. 零拷贝——比 Mercury 和 KCP 都快
     - memory-mapped buffer（AtomicBuffer）
     - 同机 IPC 路径可达 sub-microsecond 延迟

  3. 内建背压——游戏服务器最需要但最难做对的
     - Publication.offer() 在缓冲区满时返回流控错误
     - 适配 tick 模型：tick 内发不完就留到下个 tick

  4. 多播——EntityCall 广播场景天然适配
     - 属性同步广播：一个 Publication → 多个 Subscription

  5. 同机 IPC——同机房部署的杀手级优化
     - aeron:ipc 走共享内存，不经过网络栈
     - CellApp 和 BaseApp 在同一台机器时，延迟 sub-μs

  6. 成熟的 C++ SDK——不用自己写
     - 官方维护的 aeron-cpp 客户端
```

### 2.4 Aeron 集成架构

```
┌─────────────────────────────────────────────────────────┐
│ theseed Runtime                                         │
│                                                          │
│  IReliableTransport（theseed 抽象接口）                   │
│       │                                                  │
│       ├── AeronTransport（推荐，内网进程间）               │
│       │     ├── EntityCall: aeron:udp?reliable=true      │
│       │     ├── 属性同步: aeron:udp?reliable=false       │
│       │     ├── 位置更新: aeron:udp?reliable=false       │
│       │     └── 同机 IPC: aeron:ipc（共享内存）           │
│       │                                                  │
│       └── TCPTransport（备选，简单部署/跨机房）            │
│                                                          │
│  Aeron Media Driver（每台机器一个）                       │
│       ├── UDP Transport                                  │
│       ├── IPC Transport（共享内存）                       │
│       ├── 多播路由                                       │
│       └── 流控 + 重传 + 分片                              │
└─────────────────────────────────────────────────────────┘

Stream ID 分配：

  Stream ID    用途                    可靠性
  ──────────   ──────────────          ────────
  1001         EntityCall              reliable=true
  1002         属性同步（real→ghost）   reliable=true
  1003         位置/朝向更新            reliable=false
  1004         实体创建/销毁            reliable=true
  1005         迁移数据                reliable=true
  1006         AOI 进入/离开           reliable=false
  2001         控制面消息              reliable=true
  2002         心跳                    reliable=false
```

### 2.5 核心接口

```cpp
// runtime/ReliableTransport.h

class IReliableTransport {
public:
    virtual ~IReliableTransport() = default;

    virtual void send(const Address& target,
                      const Message& msg,
                      Reliability reliability) = 0;

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

class AeronTransport : public IReliableTransport {
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

        reliablePub_ = getPublication(channel, STREAM_ENTITYCALL);
        unreliablePub_ = getPublication(channel, STREAM_POSITION_UPDATE);

        reliableSub_ = getSubscription(channel, STREAM_ENTITYCALL, /*reliable=*/true);
        unreliableSub_ = getSubscription(channel, STREAM_POSITION_UPDATE, /*reliable=*/false);
    }

    void send(const Address& target,
              const Message& msg,
              Reliability reliability) override {
        auto& pub = (reliability >= Reliability::HIGH)
            ? *reliablePub_ : *unreliablePub_;

        aeron::concurrent::AtomicBuffer buffer(msg.data(), msg.size());

        std::int64_t result;
        do {
            result = pub.offer(buffer, 0, msg.size());
            if (result == aeron::Publication::BACK_PRESSURED) {
                METRIC_COUNTER("transport.backpressure", 1);
                return;  // 留到下个 tick
            }
        } while (result < 0 && result != aeron::Publication::NOT_CONNECTED);
    }

    void tick() override {
        reliableSub_->poll(messageHandler_, 100);
        unreliableSub_->poll(messageHandler_, 200);
    }

private:
    std::shared_ptr<aeron::Aeron> aeron_;
    std::shared_ptr<aeron::Publication> reliablePub_;
    std::shared_ptr<aeron::Publication> unreliablePub_;
    std::shared_ptr<aeron::Subscription> reliableSub_;
    std::shared_ptr<aeron::Subscription> unreliableSub_;
    aeron::FragmentAssembler assembler_;
};
```

### 2.7 消息头格式

```
Bundle 消息头格式（theseed）：

┌─────────────────────────────────────────┐
│ Message ID (2 bytes)                    │
│ Reliability (2 bits)                    │  ← 新增
│ Compressed (1 bit)                      │
│ Reserved (5 bits)                       │
│ Payload Length (2 bytes)                │
├─────────────────────────────────────────┤
│ Payload ...                             │
└─────────────────────────────────────────┘

发送端：
  1. 根据 MessageDescription 的可靠性配置设置标记
  2. CRITICAL/HIGH 消息进入可靠发送队列（等待 ACK）
  3. LOW/NONE 消息直接发送（不等待）

接收端：
  1. CRITICAL/HIGH 消息回复 ACK
  2. LOW/NONE 消息不回复 ACK
```

### 2.8 与两套引擎的对比

| 维度 | BigWorld Mercury | KBEngine TCP | theseed Aeron |
|------|-----------------|-------------|---------------|
| 内部协议 | UDP + 自建可靠性 | TCP | UDP + Aeron Media Driver |
| 可靠性分级 | 4 级（同一通道混用） | 无（全可靠） | reliable=true/false |
| 零拷贝 | 否 | 否 | 是 |
| 背压 | 无 | TCP 内建 | Publication.offer() 流控 |
| 多播 | 无 | 无 | UDP multicast |
| 同机优化 | UDP loopback | TCP loopback | aeron:ipc 共享内存 |
| 延迟 | 极低 | 较高 | 极低（同机 < 1μs） |
