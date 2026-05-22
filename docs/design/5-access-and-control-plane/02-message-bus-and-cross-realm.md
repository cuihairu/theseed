# MessageBus — 消息总线与跨服

> theseed 的 MessageBus 负责 Control Plane 和 Cross-Realm Async Plane。
> Runtime Data Plane 由独立的 Runtime Transport 负责，不走 MessageBus。
>
> 来源：KBEngine 无消息队列（直连 TCP），theseed 新增。
> 当前实现基线以 [../0-foundation/01-mvp-architecture-baseline](../0-foundation/01-mvp-architecture-baseline.md) 为准。

---

## 0.5 引擎实现对照与取舍

### BigWorld 是怎么实现的

```
BigWorld 没有独立的通用 MessageBus 层。

它的做法是：
  - runtime 主路径：Mercury / Mailbox / Channel
  - 运维控制面：Watcher / ForwardingWatcher
  - 跨进程管理：Manager 进程 + 内建消息协议
  - 跨服异步：没有现代 MQ 式抽象
```

### KBEngine 是怎么实现的

```
KBEngine 也没有独立消息总线。

它的做法是：
  - runtime 主路径：直连 TCP + EntityCall
  - 控制与管理：组件间自定义消息
  - 运维观察：Watcher / WebConsole
  - 跨服异步：基本没有成体系 MessageBus
```

### 优缺点

```
BigWorld / KBEngine 的优点：
  - 主路径和控制面边界天然清楚
  - 组件少，部署负担低
  - 不会误把 MQ 放进 EntityCall 主路径

BigWorld / KBEngine 的缺点：
  - 控制面广播、跨服桥接、后台任务都要自己拼
  - 缺少统一 request-reply / pub-sub / worker group 抽象
  - 跨机房与异步编排能力弱
```

### theseed 的取舍

```
theseed 不复制旧引擎“所有事情都走自定义点对点”的做法。

取舍是：
  - Runtime Data Plane 继续独立，不走 MessageBus
  - Control Plane / Cross-Realm Async Plane 单独引入 MessageBus
  - 用外部组件换取广播、桥接、异步编排和运维可管理性
```

### 为什么不是 Aeron

```
Aeron 更强在低延迟传输和流式数据面，
但它不是开箱即用的分布式消息总线：
  - 需要更多自建协议和控制面
  - 更适合 Data Plane
  - 不适合直接承担控制面语义
```

### 为什么不是 NNG

```
NNG 更像消息/传输工具箱，
而不是现成的分布式消息总线：
  - 适合点对点通信
  - 不擅长替代完整的 bus / pub-sub / request-reply 中枢
```

### 为什么不是直接自研

```
自研总线会把运维、发现、重连、路由、权限都吞进去，
成本通常高于先用成熟组件。
```

---

## 1. 为什么需要消息总线

```
没有消息总线的世界：
  控制面和跨服能力都依赖点对点连接
  发现、广播、桥接很快失控

有消息总线的世界：
  Control Plane / Cross-Realm Async Plane 拥有统一入口

额外好处：
  - 进程发现和广播简单
  - 跨服消息天然支持
  - 请求/响应模式统一
  - 异步任务分发更清晰

注意：
  MessageBus 不替代 Runtime Transport。
  实体主路径仍然由 Runtime Data Plane 负责。
```

---

## 2. 后端选择

**MVP 下 theseed 选择 NATS 作为 MessageBus 默认后端**：

| 候选 | 适配度 | 主要原因 |
|------|------|------|
| NATS | 高 | 官方原生支持 pub/sub、request-reply、queue group，适合控制面和跨服异步 |
| Aeron | 中 | 官方定位是低延迟传输、Archive、Cluster，更像 data plane / transport 层 |
| NNG | 中低 | 官方定位是 protocols + transports 工具箱，不是现成分布式 bus |
| 自研 | 低 | 要自己补发现、路由、重连、权限和运维面 |

最终选 NATS 的原因不是“最底层最快”，而是：

- 运行时主路径已由 Runtime Transport 解决
- MessageBus 更看重 subject、request-reply、queue group、跨机房桥接
- NATS 在控制面和跨服异步上更贴合需求
- 运维和部署模型也更成熟
- NATS 官方文档明确把 request-reply 和 queue groups 作为核心能力

边界如下：

1. Runtime Transport
   - EntityCall
   - Ghost / Witness 同步
   - 迁移数据

2. MessageBus
   - 控制面消息
   - 跨 Realm 异步调用
   - 后台任务分发
   - 广播 / 通知 / 离线队列

补充判断：

```
Aeron 更适合做低延迟传输或总线下层，不适合直接承担控制面语义。
NNG 更适合做协议与传输工具箱，不适合直接替代一个分布式消息中枢。
```

---

## 3. 核心接口

```cpp
class IMessageBus {
public:
    // 发布/订阅
    virtual void publish(const std::string& subject,
                         const std::vector<uint8_t>& data) = 0;
    virtual Subscription subscribe(const std::string& subject,
                                   MessageCallback callback) = 0;

    // 请求/响应
    virtual Future<BusResponse> request(const std::string& subject,
                                        const std::vector<uint8_t>& data,
                                        Duration timeout = 5s) = 0;

    // 定向发送
    virtual void send(const std::string& targetProcess,
                      const std::string& subject,
                      const std::vector<uint8_t>& data) = 0;

    // 集群广播
    virtual void broadcast(ProcessRole role,
                           const std::string& subject,
                           const std::vector<uint8_t>& data) = 0;

    virtual bool isConnected() const = 0;
    virtual BusStats getStats() const = 0;
};
```

---

## 4. 消息用途

```
MessageBus 用途
├── 控制面消息
│   ├── 进程上下线通知
│   ├── 路由表更新
│   ├── 配置变更广播
│   └── 热更命令下发
├── 异步任务
│   ├── 后台任务分发
│   ├── 离线通知
│   └── 审计日志
└── 跨服消息
    ├── 匹配
    ├── 查询
    ├── 邮件 / 通知
    └── Bridge 转发
```

```
MessageBus 禁止承载：
  - EntityCall 主路径
  - real → ghost 状态同步
  - 迁移快照数据
  - tick 内实时 AOI / 属性复制
```

---

## 5. 跨服桥接

```
Realm A                           Realm B
┌─────────────────┐              ┌─────────────────┐
│ NATS Cluster A  │◄── Bridge ──►│ NATS Cluster B  │
│ BaseApp_1       │              │ BaseApp_1       │
│ CellApp_1       │              │ CellApp_1       │
└─────────────────┘              └─────────────────┘

Bridge 配置：
  - 只转发 cross-realm 前缀的消息
  - OTLP 兼容（trace context 传播）
  - 支持请求/响应
  - 不承载实体本地权威路径
```

### 跨服脚本 API

```python
import theseed

class Avatar(theseed.BaseEntity):
    def startCrossMatch(self):
        theseed.cross_server.match(
            mode="pvp_3v3",
            player_info={"uid": self.id, "mmr": self.mmr},
            callback=self.onMatchResult
        )

    def checkFriendOnline(self, friend_uid):
        result = yield theseed.cross_server.query(
            realm="realm_b",
            method="Player.checkOnline",
            args={"uid": friend_uid}
        )

    def sendCrossServerMail(self, target_realm, target_uid, mail):
        theseed.cross_server.send_message(
            realm=target_realm,
            method="Mail.receive",
            args={"uid": target_uid, "mail": mail},
            guaranteed=True
        )
```

### CrossServerManager

```cpp
class CrossServerManager {
public:
    Future<CrossResult> call(const std::string& realm, ...);
    Future<MigrationResult> migrateEntity(EntityId id, ...);
    void sendMessage(const std::string& realm, ...);
    Future<QueryResult> query(const std::string& realm, ...);
    std::vector<RealmInfo> discoverRealms(const std::string& service);
};
```

### Realm Bridge

```
职责：
  1. 消息格式转换
  2. Trace Context 传播
  3. 消息持久化
  4. 认证
  5. 流控

实现：
  同机房：NATS Leafnode
  跨机房：NATS Gateway 或自定义 TCP bridge
```
