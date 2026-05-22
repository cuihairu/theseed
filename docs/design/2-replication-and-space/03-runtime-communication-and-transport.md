# Runtime Communication & Transport — EntityCall、运行时数据面与传输语义

> 来源：BigWorld `Mailbox / Mercury / Channel / Bundle`，
> KBEngine `EntityCall / TCP`，
> theseed 在此基础上补上更明确的运行时数据面边界。

---

## 0.5 引擎实现对照与取舍

### BigWorld 是怎么实现的

```
BigWorld 的运行时通信核心是 Mercury / Mailbox / Channel / Bundle：
  - 以通道和邮箱表达实体路由
  - 运行时消息和连接状态强绑定
  - 适合高频实体主路径
```

### KBEngine 是怎么实现的

```
KBEngine 更直接：
  - EntityCall + TCP 直连
  - 运行时语义更少
  - 更容易理解，但系统层抽象也更薄
```

### 优缺点

```
BigWorld / KBEngine 的优点：
  - 数据面和实体路由天然贴近
  - 不需要额外引入总线才跑得起来

共同缺点：
  - 控制面、异步面、数据面容易混在一起
  - 后期扩展跨 Realm 协调会变得别扭
```

### theseed 的取舍

```
theseed 继续把 Runtime Data Plane 和控制面拆开，
数据面保留面向实体路由的语义，
但不让控制/运维消息混进主路径。
```

### 为什么不能继续混层

```
一旦把 Runtime Data Plane 和 Control Plane 混成一层，
很快就会出现三种错误：
  1. 把 EntityCall 当成通用 RPC
  2. 把 MQ hop 塞进 tick 内主路径
  3. 把运维广播和实体有序消息混用同一背压语义
```

---

## 0. 设计边界

本篇负责：

```
  - EntityCall 的本质
  - Runtime Data Plane 的职责
  - 运行时送达语义
  - piggyback / overflow / inactivity 一类系统边界
  - 传输实现选型与抽象接口
```

本篇不负责：

```
  - MessageBus 控制面语义
  - 跨 Realm 异步桥接
  - 登录接入流量
```

后两者统一放在：

```
../5-access-and-control-plane
```

---

## 1. 先把三条消息平面分开

theseed 当前文档最重要的边界之一，就是把三条消息平面拆开：

### 1.1 Runtime Data Plane

负责：

```
  - EntityCall
  - real → ghost
  - migration snapshot
  - runtime create / destroy / route update
```

要求：

```
  - 明确顺序语义
  - 可感知实体路由
  - 具备背压能力
```

### 1.2 Control Plane

负责：

```
  - 进程上下线
  - 路由表发布
  - 配置广播
  - 受控停服 / 运维命令
```

### 1.3 Cross-Realm Async Plane

负责：

```
  - 跨服查询
  - 跨服通知
  - 匹配 / 邮件 / 异步编排
```

所以结论是：

```
EntityCall 不走 MessageBus。
```

这不是实现偏好，而是引擎边界。

---

## 2. EntityCall 的真实语义

EntityCall 不是通用 RPC 框架，而是：

```
“另一个进程上某个实体权威实例的代理句柄”
```

### 2.1 句柄模型

```cpp
class EntityCall {
public:
    EntityCall(EntityId id, ComponentId target, const std::string& entityType);

    void call(const std::string& method, const Args& args);
    void callWithCallback(const std::string& method,
                          const Args& args,
                          CallbackPtr callback);

    ComponentId targetComponent() const;
    void updateTarget(ComponentId newTarget);

    bool isValid() const;
    const Address& address() const;
};
```

### 2.2 为什么不能走 MQ

```
1. 同一实体的调用具备顺序依赖
2. 迁移期间路由会动态切换
3. tick 内路径不允许额外 broker hop
4. 运行时背压必须回到引擎自身
```

这也是 BigWorld `Mailbox / Mercury` 和 KBEngine `EntityCall` 的共同系统前提。

反过来说：

```
如果某条消息不需要：
  - 实体权威路由
  - tick 内顺序依赖
  - migration 窗口一致性

那它大概率就不属于 Runtime Data Plane。
```

---

## 3. 运行时送达语义

### 3.1 MVP 只承诺两类业务可见语义

```cpp
enum class DeliveryClass : uint8_t {
    ORDERED_RELIABLE = 0,
    UNORDERED_LOSSY  = 1,
};
```

适用范围：

```
ORDERED_RELIABLE：
  - EntityCall
  - create / destroy
  - migration
  - ghost 核心状态

UNORDERED_LOSSY：
  - volatile 位置 / 朝向
  - 可被新状态覆盖的辅助更新
```

### 3.2 这不等于 BigWorld 只剩两级

BigWorld 真正的系统面还包括：

```
driver / passenger / critical / none
piggyback
overflow
inactivity
channel / bundle 的背压与断链语义
```

这轮重组把这些边界留在同一篇里，而不是继续散落在两个目录。

---

## 4. BigWorld 级可靠性边界

### 4.1 piggyback

`piggyback` 不只是“顺手带点数据”，而是：

```
在已有消息流上复用确认、附带小负载、减少额外包成本的系统语义
```

### 4.2 overflow

`overflow` 不是日志事件，而是：

```
发送窗口、队列或 bundle 容量被打满时，
运行时必须进入明确的丢弃、降级或重试路径。
```

### 4.3 inactivity

`inactivity` 也不是简单超时日志，它意味着：

```
连接或 channel 的活性状态已经影响路由、恢复或重连判断。
```

这些边界在 BigWorld 里都不是“传输库实现细节”。

在 theseed 里也必须继续保留为 runtime 语义。

---

## 5. 传输抽象

### 5.1 接口

```cpp
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

### 5.2 推荐实现

theseed 仍推荐：

```
MVP：
  - Runtime Data Plane 统一走 IRuntimeTransport
  - 默认实现可优先评估 Aeron

备选：
  - TCPTransport 用于简化部署
```

原因不是“追求新技术”，而是：

```
1. 同机 IPC 路径有价值
2. Publication 背压可观测
3. 运行时可自行决定 lossy 合并与降级
```

---

## 6. Channel / Bundle 的系统角色

在 BigWorld / KBEngine 里，`Channel / Bundle` 从来不只是序列化缓冲区。

它们表达的是：

```
  - 顺序归属
  - 窗口与发送节奏
  - piggyback 位置
  - overflow / retry 决策点
```

所以 theseed 文档不应把它们写成：

```
一个薄薄的“消息包对象”
```

更合理的表述是：

```
Bundle = 运行时消息聚合单元
Channel = 到某个目标路由的状态化发送上下文
```

---

## 7. 与迁移、HA、生命周期的关系

### 7.1 与迁移

迁移窗口需要：

```
  - 路由切换前后可识别 epoch
  - 未完成消息有明确处理策略
  - 旧路由上的消息可转发、拒绝或丢弃
```

### 7.2 与 HA

HA 不只关心“快照存在”，还关心：

```
主实例故障时，
哪些 runtime channel 状态需要重建、
哪些未完成消息可以重放、
哪些必须靠上层语义补偿。
```

### 7.3 与 lifecycle

在 `drain / retire / controlled shutdown` 过程中：

```
runtime transport 也需要进入受控收敛状态，
而不是一直像稳定期一样无限接收新负载。
```

---

## 8. 分阶段边界

```
MVP：
  - EntityCall 统一走 Runtime Data Plane
  - ORDERED_RELIABLE / UNORDERED_LOSSY 两类业务可见语义
  - 基础背压与统计

Phase 2：
  - 更清晰的 channel / bundle 状态机
  - piggyback / overflow / inactivity 的显式策略接口
  - 与 migration / lifecycle 更紧密联动

Phase 3：
  - 更接近 BigWorld 的完整可靠性角色模型
  - 更成熟的运行时降级与恢复策略
```

---

## 9. 与 BigWorld / KBEngine / theseed 的对比

| 维度 | BigWorld | KBEngine | theseed |
|------|------|------|------|
| 实体远程句柄 | Mailbox | EntityCall | EntityCall |
| 主路径消息平面 | Mercury 直连 | TCP 直连 | Runtime Data Plane |
| MQ 承载主路径 | 否 | 否 | 否 |
| 可靠性系统面 | 完整且细粒度 | 相对简化 | MVP 收敛，边界保留 |
| piggyback / overflow / inactivity | 有 | 弱 | 明确写入设计边界 |

---

## 10. 一句话判断

这篇的目标不是证明：

```
theseed 已经等价实现 Mercury
```

而是明确：

```
运行时通信必须作为“复制与空间”层的一部分建模，
并且 BigWorld 式可靠性系统面已经在文档边界中被单独立住。
```
