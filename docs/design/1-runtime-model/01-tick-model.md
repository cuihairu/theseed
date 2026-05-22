# Tick Model — 主循环模型

> theseed 引擎的主循环设计。所有游戏逻辑、网络处理、属性同步都在 tick 中执行。
>
> 来源：KBEngine `EventDispatcher::processUntilBreak` + BigWorld 主循环。
> 当前实现基线以 [0-foundation/01-mvp-architecture-baseline](../0-foundation/01-mvp-architecture-baseline.md) 为准。

---

## 0.5 引擎实现对照与取舍

### BigWorld 是怎么实现的

```
BigWorld 以主循环 tick 为中心：
  - 网络收包
  - 逻辑执行
  - 属性同步
  - 定时器驱动

它把并行度主要放在进程级，而不是线程级。
```

### KBEngine 是怎么实现的

```
KBEngine 也以单线程 tick 为核心：
  - EventDispatcher 驱动事件
  - Entity 逻辑在 tick 内串行推进
  - 多进程扩展承担横向并行
```

### 优缺点

```
共同优点：
  - 确定性强
  - 脚本侧心智负担低
  - 更适合高频实体状态更新

共同缺点：
  - 单进程内并行度有限
  - 复杂计算必须外拆到异步或别的进程
```

### theseed 的取舍

```
theseed 选择继承单线程 tick 模型，
因为它是 BigWorld / KBEngine runtime 主路径的共同底座。

差异只放在：
  - 任务拆分
  - 异步边界
  - 控制面与数据面的分层
```

---

## 1. 核心问题

```
一个实体存在于服务器集群中，它需要：

1. 在正确的进程上运行（Base 或 Cell）
2. 在正确的空间中有位置（Cell 侧）
3. 被正确的客户端看到（Witness）
4. 被跨边界的邻居感知到（Ghost）
5. 属性变更能高效同步给所有相关方
6. 可以安全地跨进程迁移
7. 状态能持久化和恢复
```

这 7 件事决定了 Runtime Core 的所有组件。

---

## 2. 为什么是单线程 tick

KBEngine 和 BigWorld 都是 **单线程 tick 模型**。这不是落后，而是游戏逻辑的正确抽象：

```
原因 1：确定性
  - 游戏逻辑必须可预测：同一输入 → 同一输出
  - 多线程并发写同一实体会打破这个保证
  - 锁的开销在高频 tick（10-20Hz）下不可接受

原因 2：简单性
  - 脚本层不需要关心线程安全
  - Python GIL 本身就是单线程
  - 开发者心智负担最低

原因 3：性能
  - 单线程无锁，cache 友好
  - 游戏逻辑通常不是 CPU 瓶颈（网络 I/O 和 AOI 才是）
  - 真正的并行靠分布式（多进程），不是多线程
```

---

## 3. Tick 生命周期

```
┌─────────────────────────────────────────────────────────┐
│  一个 Tick (典型 10Hz → 100ms 预算)                       │
│                                                          │
│  1. Network Ingress                                      │
│     ├─ 接收所有就绪消息                                    │
│     ├─ 反序列化到消息队列                                  │
│     ├─ 投递上一轮异步任务完成事件                            │
│     └─ 按 Channel / Entity 顺序分发                         │
│                                                          │
│  2. Timer                                                │
│     ├─ 触发到期定时器                                      │
│     └─ 将回调加入当前 tick 的执行队列                        │
│                                                          │
│  3. Entity Logic                                         │
│     ├─ AOI 节点移动                                        │
│     ├─ Controller tick                                   │
│     ├─ 运行 C++ 核心逻辑                                   │
│     └─ 只允许 owning tick thread 改实体状态                │
│                                                          │
│  4. Script Callback                                      │
│     ├─ 执行消息触发的脚本回调                                │
│     ├─ 执行定时器触发的脚本回调                              │
│     └─ 属性变更只标 dirty，不立即外发                        │
│                                                          │
│  5. Sync Build                                           │
│     ├─ 收集生命周期事件                                    │
│     ├─ 构造 real → ghost state-delta Bundle              │
│     ├─ 构造 witness → client Bundle                      │
│     └─ 计算本 tick 的 flush 列表                           │
│                                                          │
│  6. Flush                                                │
│     ├─ flush Runtime Data Plane                          │
│     ├─ flush Client Channel                              │
│     └─ 更新 tick 统计与背压指标                             │
│                                                          │
│  7. Idle                                                 │
│     └─ 等待下一个 tick 唤醒                                 │
└─────────────────────────────────────────────────────────┘
```

### 3.1 关键约束

```
MVP 时序规则：

  - tick 内允许改属性
  - tick 内只记脏，不外发
  - tick 末统一构造同步数据
  - tick 末统一 flush

例外只有：
  - 明确的 RPC / 事件消息
  - 创建 / 销毁 / 迁移等生命周期控制消息
```

### 3.2 与异步任务的边界

```
异步任务可以在后台线程执行，但结果不能直接写 Entity。

必须：
  - 先投递完成事件到 owning tick thread
  - 在下一次 Network Ingress 或事件分发阶段进入主循环

禁止：
  - 在后台线程直接 setProperty()
  - 在 tick 线程阻塞等待 Future.get()
```

---

## 4. 核心接口

```cpp
// runtime/TickModel.h

class ITickable {
public:
    virtual ~ITickable() = default;
    virtual void tick(Duration deltaTime) = 0;
};

class TickScheduler {
public:
    void registerTickable(ITickable* obj, TickPhase phase);

    void run();
    void stop();

    uint64_t currentTick() const;
    Duration tickBudget() const;
    Duration lastTickDuration() const;

private:
    enum class TickPhase {
        Network,        // 消息接收、异步完成事件投递
        Timer,          // 定时器到期
        Entity,         // C++ 核心逻辑
        Script,         // 脚本回调
        SyncBuild,      // 构造同步数据
        Flush,          // 统一发送
    };

    std::array<std::vector<ITickable*>, 6> phaseQueues_;
};
```

---

## 5. 与 KBEngine 的对应

```
KBEngine:
  EventDispatcher::processUntilBreak()
    → processNetworkRequests()     → Phase::Network
    → processTimers()              → Phase::Timer
    → Cellapp::processEntityTick() → Phase::Entity
    → script execute               → Phase::Script
    → Witness::update()            → Phase::SyncBuild / Flush

theseed:
  TickScheduler::run()
    → tick(Network)
    → tick(Timer)
    → tick(Entity)
    → tick(Script)
    → tick(SyncBuild)
    → tick(Flush)
```

```
差异点：
  - theseed 明确把 “构造同步数据” 和 “发送” 拆成两个阶段
  - 这让背压、统计、重试和调试更容易做清楚
  - 也避免把属性复制混在脚本执行路径里
```
