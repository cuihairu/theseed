# Runtime Transport Reliability — 可靠性层级、piggyback、窗口溢出与超时边界

> 来源：BigWorld Mercury `RELIABLE_NO / DRIVER / PASSENGER / CRITICAL`、`UDPBundle piggyback`、`UDPChannel overflow / inactivity / resend`。
> 这篇讲的是运行时消息可靠性语义，不是单纯“底层用 UDP 还是 TCP”。

---

## 0. 设计边界

```
本篇负责：
  - 运行时消息可靠性层级
  - piggyback / resend / window overflow / inactivity timeout
  - Bundle / Channel 语义如何影响 Runtime Data Plane

本篇不负责：
  - EntityCall 上层语义本身
  - MessageBus 控制面消息
  - 具体网络库选型 benchmarking
```

和其他文档的关系：

```
05-communication
  - 讲 Runtime Data Plane 总体分层和 MVP 收敛方案

07-entity-migration
  - 讲迁移窗口消息怎么处理

本篇
  - 讲底层可靠性语义到底要表达什么
```

---

## 1. 先纠正一个误读

BigWorld Mercury 的价值不只是：

```
UDP + 自建可靠性
```

真正重要的是它把同一条运行时通道上的消息分成了不同可靠性角色：

```
RELIABLE_NO
RELIABLE_DRIVER
RELIABLE_PASSENGER
RELIABLE_CRITICAL
```

这说明 BigWorld 解决的问题不是：

```
消息可靠 or 不可靠
```

而是：

```
消息在 bundle / resend / piggyback / overflow 体系里扮演什么角色
```

---

## 2. BigWorld 这四级到底是什么意思

### 2.1 RELIABLE_NO

```
不进入可靠传输链
丢了就丢了
```

### 2.2 RELIABLE_DRIVER

```
可靠消息的“驱动者”
会推动整个 bundle 进入可靠链
```

### 2.3 RELIABLE_PASSENGER

```
只有在同一个 UDPBundle 里已经存在至少一个 driver 时，
它才搭车进入可靠链
```

### 2.4 RELIABLE_CRITICAL

```
和 driver 一样会驱动可靠链，
同时把整个 bundle 标成 critical
```

这里最关键的点是：

```
PASSENGER 不是“不可靠”
而是“自己不单独驱动可靠发送”
```

---

## 3. 这四级解决的真实问题

BigWorld 这样设计，是因为运行时消息里常常存在：

```
1. 必须单独保证的控制消息
2. 只能跟着控制消息一起可靠到达的附属消息
3. 丢了无所谓的过期消息
4. 必须提高处理优先级的关键 bundle
```

也就是说，它在表达两类维度：

| 维度 | 问题 |
|------|------|
| Reliability trigger | 这条消息是否自己驱动可靠链 |
| Criticality | 这条消息是否把 bundle 提升为关键发送单元 |

theseed 现有文档里只保留了 “reliable / lossy” 两类，这是合理的 MVP 收缩，但必须明确这只是收缩，不等于已覆盖 BigWorld 原义。

---

## 4. piggyback 是系统语义，不是优化细节

BigWorld Mercury 里 `piggyback` 的存在说明：

```
丢包后的可靠数据
未必每次都单独重发
还可能被挂到后续正常发送的包上
```

这意味着底层运行时传输至少要考虑：

```
1. resend as standalone
2. piggyback onto next outgoing packet
3. piggyback may itself contain older piggybacks
```

这是很多“简单 UDP reliable 封装”没有认真建模的点。

---

## 5. overflow 与 inactivity 不是日志问题

BigWorld 的 `window overflow / inactivity timeout` 说明：

### 5.1 overflow

```
可靠窗口积压过大
发送侧已经失去健康状态
```

这不是普通统计值，而是：

```
运行时需要升级处理的背压事件
```

### 5.2 inactivity

```
长时间无收发活动
可能意味着链路故障、对端停滞或 channel 失活
```

所以这些都不该只出现在指标表里，而应进入 Runtime 健康模型。

---

## 6. Channel / Bundle 在表达什么

BigWorld 的 Bundle / Channel 体系至少表达了四件事：

```
1. 一组消息作为一个发送单元
2. bundle 内不同消息的可靠性角色可以不同
3. channel 维护 resend / ack / overflow / inactivity 状态
4. request / reply 的 reply order 也与 bundle 绑定
```

这说明 Runtime Data Plane 的抽象粒度不能只有：

```
send(message)
```

至少还要有：

```
send(bundle)
observe(channel health)
classify(message role)
```

---

## 7. theseed 现在的收敛方案哪里还不够

当前 [05-communication](../1-core/05-communication.md) 把 MVP 收敛成：

```
ORDERED_RELIABLE
UNORDERED_LOSSY
```

这是对的，但还需要加一句：

```
它只覆盖“可靠送达 vs 可丢弃”的最低可行语义，
没有覆盖 BigWorld 对“可靠链驱动角色 / piggyback / critical bundle”的表达能力。
```

否则文档容易让人误以为：

```
我们已经在语义层等价覆盖 Mercury
```

其实还没有。

---

## 8. theseed 的建议建模

### 8.1 分离两层概念

theseed 建议拆成两层：

| 层 | 作用 |
|------|------|
| DeliveryClass | 上层业务可见语义：保序可靠、可丢覆盖 |
| ReliabilityRole | 传输层内部角色：driver、passenger、critical、none |

这样可以在不污染业务接口的前提下，保留向 BigWorld 级语义演进的空间。

### 8.2 内部角色

```cpp
// transport/ReliabilityRole.h

enum class ReliabilityRole : uint8_t {
    None = 0,
    Driver = 1,
    Passenger = 2,
    Critical = 3,
};
```

### 8.3 Bundle 描述

```cpp
// transport/RuntimeBundleDescriptor.h

struct RuntimeBundleDescriptor {
    DeliveryClass deliveryClass = DeliveryClass::ORDERED_RELIABLE;
    ReliabilityRole reliabilityRole = ReliabilityRole::Driver;
    bool allowPiggyback = true;
    bool isCritical = false;
};
```

---

## 9. 推荐的语义映射

### 9.1 MVP

```
业务只暴露：
  ORDERED_RELIABLE
  UNORDERED_LOSSY

内部默认映射：
  ORDERED_RELIABLE → Driver
  UNORDERED_LOSSY  → None
```

### 9.2 Phase 2

开始引入内部优化角色：

```
ORDERED_RELIABLE + attached state delta
  → Driver + Passenger

shutdown / retire / migration commit
  → Critical
```

### 9.3 Phase 3

显式支持：

```
driver / passenger / critical bundle policy
piggyback budget
overflow escalation policy
```

---

## 10. piggyback / resend / overflow 的策略接口

```cpp
// transport/IReliabilityPolicy.h

struct ChannelHealthSnapshot {
    size_t unackedPackets = 0;
    size_t overflowPackets = 0;
    bool inactive = false;
};

class IReliabilityPolicy {
public:
    virtual ~IReliabilityPolicy() = default;

    virtual bool shouldPiggyback(const ChannelHealthSnapshot& health) const = 0;
    virtual bool shouldResendStandalone(
        const ChannelHealthSnapshot& health) const = 0;
    virtual bool shouldEscalateOverflow(
        const ChannelHealthSnapshot& health) const = 0;
};
```

设计要求：

```
1. piggyback 不是隐式副作用，应可观测
2. overflow 不是仅记录计数，应有升级动作
3. inactivity 不能只靠 TCP 连接状态替代
```

---

## 11. 和迁移 / HA / Lifecycle 的关系

### 11.1 和迁移

迁移窗口里的运行时消息，需要比普通位置流更强的语义：

```
migration commit / route switch
  不能只是 ORDERED_RELIABLE
  更接近 critical control bundle
```

### 11.2 和 HA / shutdown

```
backup ack / topology switch / controlled shutdown barrier
  也更接近 critical
```

### 11.3 和背压

如果 overflow 持续扩大：

```
Runtime 应触发：
  - 降级 lossy 流
  - 延迟非关键同步
  - 输出 operator 可见状态
```

这说明可靠性设计和生命周期控制其实是连着的。

---

## 12. 分阶段边界

```
MVP：
  - 业务暴露两类 DeliveryClass
  - 不承诺 BigWorld 四级角色等价实现

Phase 2：
  - 内部引入 ReliabilityRole
  - piggyback / overflow / inactivity 进入 channel health 模型

Phase 3：
  - 更接近 Mercury 的 driver / passenger / critical 语义
  - 关键 bundle、迁移、HA 控制消息精细化分级
```

---

## 13. 与 BigWorld / KBEngine / theseed 的对比

| 维度 | BigWorld Mercury | KBEngine TCP | theseed |
|------|------------------|-------------|---------|
| 业务层语义 | 多层 | 基本全可靠 | MVP 先两类 |
| 传输层内部角色 | Driver / Passenger / Critical | 无 | Phase 2/3 目标 |
| piggyback | 有 | 无 | 需显式设计 |
| overflow / inactivity 健康模型 | 有 | 较弱 | 需纳入 channel health |
| 与 HA / migration 联动 | 强 | 弱 | 需后续联动 |
