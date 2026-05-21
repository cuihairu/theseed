# MessageBus — 消息总线与跨服

> theseed 消息总线：Aeron 统一 Runtime Transport 和控制面消息。
> 跨服桥接用 NATS。
>
> 来源：KBEngine 无消息队列（直连 TCP），theseed 新增。

---

## 1. 为什么需要消息总线

```
没有消息总线的世界：
  BaseApp_A ←→ CellApp_3, CellApp_7 ...（N×M 连接）

有消息总线的世界：
  所有进程只连 MessageBus（拓扑清晰）

额外好处：
  - 跨服消息天然支持
  - 广播消息一条到达所有进程
  - 持久化消息（离线队列）
  - 异步任务分发
```

---

## 2. 后端选择

**theseed 选择 Aeron 作为默认**：
- 进程间 EntityCall 和属性同步已用 Aeron（见 1-core/05-communication）
- 控制面消息复用同一个 Media Driver
- 跨服桥接用 NATS（跨机房 Aeron 集群管理复杂）
- Aeron Archive 可选做持久化消息

**Aeron 双重角色**：
1. Runtime Transport（EntityCall、属性同步、位置更新）
2. MessageBus（控制面、事件广播、异步任务分发）
同一个 Media Driver，不同 streamId 区分用途。

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
├── 实时消息
│   ├── EntityCall 跨进程转发
│   ├── 属性同步广播
│   ├── AOI 更新通知
│   └── 跨服路由
├── 控制面消息
│   ├── 进程上下线通知
│   ├── 路由表更新
│   ├── 配置变更广播
│   └── 热更命令下发
└── 持久化消息
    ├── 离线消息队列
    ├── 异步任务分发
    └── 审计日志
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

脚本层：
  entity.call_remote("realm_b", "Player.onCrossRealmEvent", args)
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
