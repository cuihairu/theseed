# Tick Model — 主循环模型

> theseed 引擎的主循环设计。所有游戏逻辑、网络处理、属性同步都在 tick 中执行。
>
> 来源：KBEngine `EventDispatcher::processUntilBreak` + BigWorld 主循环。

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
│  1. Network Process                                      │
│     ├─ 接收所有就绪消息                                    │
│     ├─ 反序列化到 Message 队列                              │
│     └─ 按 Channel 顺序分发                                 │
│                                                          │
│  2. Timer Callbacks                                      │
│     ├─ 到期的定时器触发回调                                  │
│     └─ 按注册顺序执行                                      │
│                                                          │
│  3. Entity Processing (CellApp only)                     │
│     ├─ AOI 更新（坐标节点移动 → 触发器事件）                  │
│     ├─ Witness 更新（视野进出 + 属性同步）                   │
│     ├─ 移动控制器 tick                                     │
│     └─ Ghost 同步（real → ghost 属性推送）                  │
│                                                          │
│  4. Script Execution                                     │
│     ├─ 消息触发的脚本回调                                    │
│     ├─ 定时器触发的脚本回调                                  │
│     └─ 所有脚本回调在主线程同步执行                           │
│                                                          │
│  5. Sync & Flush                                         │
│     ├─ 收集所有脏属性                                      │
│     ├─ 构造同步 Bundle                                     │
│     ├─ 发送给 Ghost                                        │
│     ├─ 发送给 Witness（客户端）                              │
│     └─ flush 所有 Channel                                  │
│                                                          │
│  6. Idle                                                 │
│     └─ 等待下一个 tick 唤醒                                 │
└─────────────────────────────────────────────────────────┘
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
    // 注册 tick 对象
    void registerTickable(ITickable* obj, TickPhase phase);

    // 主循环
    void run();
    void stop();

    // 当前 tick 信息
    uint64_t currentTick() const;
    Duration tickBudget() const;        // 配置的 tick 预算
    Duration lastTickDuration() const;  // 上次 tick 实际耗时

private:
    // tick 阶段（按顺序执行）
    enum class TickPhase {
        Network,        // 网络消息处理
        Timer,          // 定时器回调
        Entity,         // 实体处理（AOI/Witness/Movement）
        Script,         // 脚本执行
        Sync,           // 同步 & flush
    };

    std::array<std::vector<ITickable*>, 5> phaseQueues_;
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
    → Witness::update()            → Phase::Sync

theseed:
  TickScheduler::run()
    → tick(Network)
    → tick(Timer)
    → tick(Entity)
    → tick(Script)
    → tick(Sync)
```
