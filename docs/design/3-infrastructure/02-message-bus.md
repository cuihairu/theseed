# MessageBus — 消息总线与跨服

> theseed 的 MessageBus 负责 Control Plane 和 Cross-Realm Async Plane。
> Runtime Data Plane 由独立的 Runtime Transport 负责，不走 MessageBus。
>
> 来源：KBEngine 无消息队列（直连 TCP），theseed 新增。
> 当前实现基线以 [../0-foundation/01-mvp-architecture-baseline](../0-foundation/01-mvp-architecture-baseline.md) 为准。

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

- 运行时主路径已由 Runtime Transport 解决
- MessageBus 更看重 subject、request-reply、queue group、跨机房桥接
- NATS 在控制面和跨服异步上更贴合需求
- 运维和部署模型也更成熟

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
