# Infrastructure Layer Design — 开箱即用的基础设施

> theseed 的基础设施层设计目标：开发者启动项目后不需要自己搭建消息队列、Redis、日志收集、网关，
> 这些全部内建且可配置。同时为跨服场景提供便利封装。

---

## 1. 整体架构

```
┌─────────────────────────────────────────────────────────────────────┐
│  Clients (Unity / Unreal / Godot / Web / 自定义)                    │
└───────────────────────────┬─────────────────────────────────────────┘
                            │
                    ┌───────▼────────┐
                    │   Gateway      │ ← 开箱即用网关层
                    │   (LoginProxy) │    TLS 终结、限流、认证、路由
                    └───────┬────────┘
                            │
        ┌───────────────────┼───────────────────┐
        │                   │                   │
  ┌─────▼─────┐      ┌─────▼─────┐      ┌──────▼──────┐
  │  BaseApp   │      │  CellApp   │      │  BaseApp    │
  │  (logic)   │      │  (space)   │      │  (logic)    │
  └─────┬──────┘      └─────┬──────┘      └──────┬──────┘
        │                   │                   │
        └───────────────────┼───────────────────┘
                            │
        ┌───────────────────┼───────────────────┐
        │                   │                   │
  ┌─────▼─────┐     ┌──────▼──────┐     ┌──────▼──────┐
  │  Redis    │     │  MessageBus │     │  DBMgr      │
  │  (cache)  │     │  (NATS/     │     │  (MySQL/PG/ │
  │           │     │   Aeron)    │     │   Mongo)    │
  └───────────┘     └─────────────┘     └─────────────┘
```

---

## 2. Gateway 网关层

### 2.1 设计目标

- **开箱即用**：不需要额外部署 Nginx/Envoy，引擎自带网关
- **安全边界**：TLS 终结、客户端认证、限流、IP 黑白名单
- **路由能力**：根据玩家 UID 路由到对应 BaseApp
- **协议支持**：TCP / WebSocket / HTTP (REST)

### 2.2 架构

```
Client
  │
  ├─ TCP (游戏协议) ──┐
  ├─ WebSocket ───────┤
  │                    │
  └─ HTTP (REST API) ─┤
                       │
                ┌──────▼──────────┐
                │    Gateway       │
                │                  │
                │  ┌────────────┐  │
                │  │ TLS 终结    │  │  ← 证书自动管理 (Let's Encrypt / 手动)
                │  └─────┬──────┘  │
                │        │         │
                │  ┌─────▼──────┐  │
                │  │ 认证层      │  │  ← Token / Session / HMAC
                │  └─────┬──────┘  │
                │        │         │
                │  ┌─────▼──────┐  │
                │  │ 限流层      │  │  ← 令牌桶 / 滑动窗口
                │  └─────┬──────┘  │
                │        │         │
                │  ┌─────▼──────┐  │
                │  │ 路由层      │  │  ← UID hash → BaseApp
                │  └─────┬──────┘  │
                │        │         │
                └────────┼─────────┘
                         │
                  转发到内部进程
```

### 2.3 核心接口

```cpp
// gateway/IGateway.h

class IGateway {
public:
    virtual ~IGateway() = default;

    // 启动网关
    virtual void start(const GatewayConfig& config) = 0;
    virtual void stop() = 0;

    // 路由策略
    virtual void setRouter(std::shared_ptr<IRouter> router) = 0;

    // 连接管理
    virtual size_t activeConnections() const = 0;
    virtual void kickConnection(ConnectionId id, const std::string& reason) = 0;
    virtual void kickAll(const std::string& reason) = 0;

    // 限流
    virtual void setRateLimit(const RateLimitConfig& config) = 0;

    // 统计
    virtual GatewayStats getStats() const = 0;
};

// 路由策略
class IRouter {
public:
    virtual ~IRouter() = default;

    // 根据认证信息决定路由到哪个 BaseApp
    virtual ServerEndpoint route(const AuthInfo& auth) = 0;

    // 路由表更新（BaseApp 上下线时调用）
    virtual void updateRouteTable(const std::vector<ServerEndpoint>& endpoints) = 0;
};
```

### 2.4 配置示例

```yaml
# config/gateway.yaml

gateway:
  # 监听
  listeners:
    - protocol: tcp
      port: 20000
      tls: false                # 开发环境不需要 TLS

    - protocol: websocket
      port: 20001
      tls: false

    - protocol: http
      port: 20002
      cors:
        origins: ["*"]
        methods: ["GET", "POST"]

  # TLS (生产环境)
  tls:
    cert_file: /etc/theseed/certs/server.pem
    key_file: /etc/theseed/certs/server.key

  # 认证
  auth:
    method: token               # token / session / hmac
    token_header: "X-Theseed-Token"
    token_validator: "script:validateToken"  # 脚本函数名

  # 限流
  rate_limit:
    global: 100000              # 全局最大并发连接
    per_ip: 100                 # 每 IP 最大连接数
    per_second: 1000            # 每秒最大新连接
    message_per_second: 100     # 每连接每秒最大消息数

  # 路由
  router:
    method: consistent_hash     # consistent_hash / round_robin / script
    hash_key: uid               # 基于 UID 一致性哈希
    virtual_nodes: 150          # 虚拟节点数
```

---

## 3. MessageBus 消息总线

### 3.1 为什么需要消息总线

```
没有消息总线的世界：
  BaseApp_A ←直接连接→ CellApp_3
  BaseApp_A ←直接连接→ CellApp_7
  BaseApp_B ←直接连接→ CellApp_3
  ...（N×M 连接，拓扑混乱）

有消息总线的世界：
  BaseApp_A → MessageBus → CellApp_3
  BaseApp_B → MessageBus → CellApp_7
  ...（每个进程只连 MessageBus，拓扑清晰）

额外好处：
  - 跨服消息天然支持（不同服的消息总线可以桥接）
  - 广播消息（world announcement）一条消息到达所有进程
  - 持久化消息（离线消息队列）
  - 异步任务分发
```

### 3.2 后端选择

| 后端 | 优势 | 劣势 | 适用场景 |
|------|------|------|---------|
| NATS | 超低延迟（sub-ms）、轻量、At-Most-Once | 不支持 Exactly-Once | 实时游戏消息 |
| NATS JetStream | 在 NATS 基础上加持久化 | 比 NATS 稍慢 | 需要持久化的消息 |
| Aeron | 零拷贝、极致性能 | 重量级、学习曲线高 | 超高频交易级 |
| Redis Streams | 复用已有 Redis | 延迟较高（ms 级） | 已有 Redis 的项目 |

**theseed 默认选择 NATS**：性能足够、部署简单、社区活跃、支持集群。

### 3.3 核心接口

```cpp
// messagebus/IMessageBus.h

class IMessageBus {
public:
    virtual ~IMessageBus() = default;

    // 发布/订阅（主题广播）
    virtual void publish(const std::string& subject,
                         const std::vector<uint8_t>& data) = 0;
    virtual Subscription subscribe(const std::string& subject,
                                   MessageCallback callback) = 0;

    // 请求/响应（点对点）
    virtual Future<BusResponse> request(const std::string& subject,
                                        const std::vector<uint8_t>& data,
                                        Duration timeout = 5s) = 0;

    // 定向发送（发给特定进程）
    virtual void send(const std::string& targetProcess,
                      const std::string& subject,
                      const std::vector<uint8_t>& data) = 0;

    // 集群广播（所有同类进程）
    virtual void broadcast(ProcessRole role,
                           const std::string& subject,
                           const std::vector<uint8_t>& data) = 0;

    // 健康检查
    virtual bool isConnected() const = 0;
    virtual BusStats getStats() const = 0;
};

// 订阅句柄（RAII，析构自动取消订阅）
class Subscription {
public:
    ~Subscription() { unsubscribe(); }
    void unsubscribe();
};
```

### 3.4 这些消息都用在哪些场景

```
MessageBus 用途
├── 实时消息（NATS Core）
│   ├── EntityCall 跨进程转发（点对点）
│   ├── 属性同步广播（同类进程广播）
│   ├── AOI 更新通知（跨 CellApp）
│   └── 跨服路由（不同 realm 间桥接）
│
├── 控制面消息（NATS Core）
│   ├── 进程上下线通知
│   ├── 路由表更新
│   ├── 配置变更广播
│   └── 热更命令下发
│
└── 持久化消息（NATS JetStream）
    ├── 离线消息队列（玩家不在线时暂存）
    ├── 异步任务分发
    └── 审计日志（保证不丢）
```

### 3.5 跨服桥接

```
Realm A (游戏世界 A)              Realm B (游戏世界 B)
┌─────────────────┐              ┌─────────────────┐
│ NATS Cluster A  │◄── Bridge ──►│ NATS Cluster B  │
│                 │              │                 │
│ BaseApp_1       │              │ BaseApp_1       │
│ CellApp_1       │              │ CellApp_1       │
└─────────────────┘              └─────────────────┘

Bridge 配置：
  - 只转发 cross-realm 前缀的消息
  - 消息格式：OTLP 兼容（trace context 可传播）
  - 支持请求/响应（跨服查询玩家数据等）

脚本层使用：
  # 跨服调用和本服调用 API 一致
  entity.call_remote("realm_b", "Player.onCrossRealmEvent", args)
  # 框架自动：MessageBus → Bridge → 远端 MessageBus → 目标 Entity
```

---

## 4. Redis 集成

### 4.1 用途

```
Redis 在 theseed 中的角色
├── 缓存层
│   ├── 实体热数据缓存（减少 DB 查询）
│   ├── 玩家在线状态
│   └── 配置缓存
│
├── 分布式锁
│   ├── 跨进程操作互斥
│   ├── 分布式定时器（防止多进程重复执行）
│   └── 实体迁移锁
│
├── 排行榜
│   ├── Sorted Set 实现实时排行榜
│   └── 支持多维度排名
│
├── 发布/订阅
│   ├── 轻量级进程间通知
│   └── 配置变更推送
│
├── 限流
│   ├── 令牌桶 / 滑动窗口
│   └── 分布式限流（网关层使用）
│
└── 会话存储
    ├── 玩家登录 Token
    ├── 临时会话数据
    └── 断线重连 session
```

### 4.2 核心接口

```cpp
// cache/IRedisProvider.h

class IRedisProvider {
public:
    virtual ~IRedisProvider() = default;

    // 基础操作
    virtual Future<std::string> get(const std::string& key) = 0;
    virtual Future<void> set(const std::string& key,
                             const std::string& value,
                             Duration ttl = Duration::zero()) = 0;
    virtual Future<bool> del(const std::string& key) = 0;
    virtual Future<bool> exists(const std::string& key) = 0;

    // Hash 操作
    virtual Future<std::string> hget(const std::string& key,
                                     const std::string& field) = 0;
    virtual Future<void> hset(const std::string& key,
                              const std::string& field,
                              const std::string& value) = 0;
    virtual Future<std::unordered_map<std::string, std::string>>
        hgetall(const std::string& key) = 0;

    // Sorted Set 操作（排行榜）
    virtual Future<void> zadd(const std::string& key,
                              const std::string& member,
                              double score) = 0;
    virtual Future<std::vector<RankEntry>> zrange(const std::string& key,
                                                   int64_t start,
                                                   int64_t stop,
                                                   bool withScores = true) = 0;
    virtual Future<int64_t> zrank(const std::string& key,
                                  const std::string& member) = 0;

    // 分布式锁
    virtual Future<LockHandle> lock(const std::string& key,
                                    Duration ttl,
                                    Duration waitTimeout = 5s) = 0;

    // 原子操作
    virtual Future<int64_t> incr(const std::string& key) = 0;
    virtual Future<int64_t> incrby(const std::string& key, int64_t delta) = 0;

    // Pipeline（批量操作）
    virtual Future<std::vector<RedisReply>> pipeline(
        const std::vector<RedisCommand>& commands) = 0;

    // Lua 脚本
    virtual Future<RedisReply> eval(const std::string& script,
                                    const std::vector<std::string>& keys,
                                    const std::vector<std::string>& args) = 0;

    // 连接池信息
    virtual RedisPoolStats getPoolStats() const = 0;
};
```

### 4.3 配置

```yaml
# config/redis.yaml

redis:
  # 单机模式
  mode: standalone
  host: 127.0.0.1
  port: 6379
  password: ""

  # 集群模式
  # mode: cluster
  # nodes:
  #   - redis-1:6379
  #   - redis-2:6379
  #   - redis-3:6379

  # 连接池
  pool:
    min_connections: 4
    max_connections: 32
    connect_timeout: 3s
    socket_timeout: 1s

  # 重试
  retry:
    max_attempts: 3
    initial_delay: 100ms
    max_delay: 1s

  # key 前缀（多环境隔离）
  key_prefix: "theseed:prod:"
```

### 4.4 脚本层暴露

```python
# Python 脚本中使用 Redis
import theseed

# 缓存
theseed.redis.set("player:10042:last_login", "2026-05-19", ttl=3600)
last_login = theseed.redis.get("player:10042:last_login")

# 排行榜
theseed.redis.zadd("leaderboard:daily_kill", "player:10042", 128)
top10 = theseed.redis.zrange("leaderboard:daily_kill", 0, 9)

# 分布式锁（with 语法自动释放）
with theseed.redis.lock("guild_war:match", ttl=30) as lock:
    if lock.acquired:
        do_match()

# 原子计数器
online_count = theseed.redis.incr("server:online_count")
```

---

## 5. 异步任务框架

### 5.1 设计目标

- 游戏逻辑天然是异步的（DB 查询、RPC、定时器）
- 提供统一的异步编程模型，不依赖回调地狱
- 支持 C++ 和脚本层

### 5.2 C++ 异步模型

```cpp
// async/AsyncFramework.h

// --- Future / Promise ---
template<typename T>
class Future {
public:
    // 链式调用
    template<typename Func>
    Future<std::invoke_result_t<Func, T>> then(Func&& f);

    // 错误处理
    Future<T> onError(std::function<T(const Error&)> handler);

    // 超时
    Future<T> timeout(Duration d);

    // 等待（阻塞，慎用）
    T get(Duration timeout = Duration::max());

    // 是否完成
    bool isReady() const;
};

// --- AsyncExecutor ---
class IAsyncExecutor {
public:
    virtual ~IAsyncExecutor() = default;

    // 在 I/O 线程执行
    virtual void post(Task task) = 0;

    // 延迟执行
    virtual void postDelayed(Task task, Duration delay) = 0;

    // 定时执行（游戏逻辑主线程）
    virtual void postOnMainThread(Task task) = 0;

    // 取消
    virtual CancelToken scheduleRepeating(Task task, Duration interval) = 0;
};

// --- 典型使用 ---
Future<PlayerData> loadPlayer(EntityId id) {
    // 异步 DB 查询
    return storage_->load(id, PlayerDef{})
        .timeout(3s)
        .then([](PlayerData data) {
            // 在主线程处理
            return processPlayer(data);
        })
        .onError([](const Error& e) {
            LOG_ERROR("load player failed", {{"entity_id", id}, {"error", e.what()}});
            return PlayerData::default();
        });
}
```

### 5.3 脚本层异步模型

```python
# Python 脚本中的异步模型

import theseed

class Avatar(theseed.BaseEntity):
    def onLogin(self):
        # 异步加载数据
        data = yield theseed.async_.db_load("Avatar", self.id)
        self.name = data["name"]
        self.level = data["level"]

        # 异步跨服查询
        result = yield theseed.async_.cross_server_query(
            "realm_b", "Guild.getMemberCount", self.guild_id
        )

        # 异步 HTTP 请求（支付回调等）
        payment = yield theseed.async_.http_post(
            "https://pay.example.com/verify",
            json={"order_id": self.order_id}
        )
        if payment["status"] == "ok":
            self.gold += payment["amount"]

        # 异步 Redis 操作
        rank = yield theseed.async_.redis.zrank("leaderboard:level", self.id)
        self.callClient("onRankUpdated", rank)

    # 定时任务
    @theseed.timer(interval=30)
    def onSave(self):
        yield theseed.async_.db_save(self)
```

### 5.4 异步任务队列

```cpp
// async/TaskQueue.h

// 后台任务队列（适合重计算、批量操作）
class TaskQueue {
public:
    // 创建指定工作线程数的队列
    static std::shared_ptr<TaskQueue> create(const std::string& name, int workers);

    // 提交任务
    Future<T> submit(std::function<T()> task);

    // 批量提交
    Future<std::vector<T>> submitAll(std::vector<std::function<T()>> tasks);
};

// 预定义队列
// theseed 内建以下队列，脚本可直接使用：

// 1. DB 队列 - 数据库操作
auto dbQueue = TaskQueue::get("db");

// 2. IO 队列 - 文件、网络 I/O
auto ioQueue = TaskQueue::get("io");

// 3. 计算队列 - 寻路、NavMesh 等重计算
auto computeQueue = TaskQueue::get("compute");

// 4. 自定义队列
auto customQueue = TaskQueue::create("my_worker", 4);
```

---

## 6. 日志收集

### 6.1 架构

```
引擎进程
  │
  ├─ OTel SDK → OTLP Exporter ──→ OTel Collector
  │                                   │
  └─ 文件日志（fallback）             ├─ → Loki (日志存储)
       │                              ├─ → Elasticsearch (可选)
       └─ Fluent Bit Agent ───────────┘
                                       │
                                  Grafana (查询/展示)
```

### 6.2 配置

```yaml
# config/logging.yaml

logging:
  # OTel 日志（推荐）
  otel:
    enabled: true
    endpoint: "otel-collector:4317"    # OTLP gRPC
    protocol: grpc                     # grpc / http

  # 文件日志（本地 fallback）
  file:
    enabled: true
    path: "/var/log/theseed/"
    rotation:
      max_size: 100mb
      max_files: 10
      compress: true

  # 级别
  level:
    default: info
    overrides:
      "theseed.network": warn          # 网络层只记录 warn 以上
      "theseed.aoi": warn              # AOI 高频日志降低级别
      "theseed.script": debug          # 脚本层开发时 debug

  # 结构化字段（自动附加到每条日志）
  default_attributes:
    cluster: "prod-cn-east"
    zone: "zone-a"
```

### 6.3 日志查询示例

```json
// Grafana/Loki 查询：查找特定玩家的所有日志
{service="theseed-baseapp"} |= "entity.id=10042"

// 查找所有错误日志
{service=~"theseed-.*"} | json | severity="ERROR"

// 查找特定 trace 的所有日志（traces + logs 联动）
{service=~"theseed-.*"} | json | trace_id="abc123def456"

// 查找慢 tick
{service="theseed-cellapp"} | json | message="tick.slow"
```

---

## 7. 跨服便利性

### 7.1 跨服场景封装

```python
# 跨服在脚本层的 API 设计

import theseed

class Avatar(theseed.BaseEntity):
    # === 场景 1: 跨服匹配 ===
    def startCrossMatch(self):
        """申请跨服匹配"""
        theseed.cross_server.match(
            mode="pvp_3v3",
            player_info={"uid": self.id, "mmr": self.mmr, "level": self.level},
            callback=self.onMatchResult
        )

    def onMatchResult(self, result):
        """匹配成功，被拉入跨服战斗"""
        battle_server = result.server       # 远端服务器信息
        battle_room = result.room_id
        # 框架自动处理：
        # 1. 在目标服创建代理实体
        # 2. 通知客户端切换连接（如果需要）
        # 3. 本地实体进入挂起状态
        self.enterCrossServer(battle_server, battle_room)

    # === 场景 2: 跨服查询 ===
    def checkFriendOnline(self, friend_uid):
        """查询好友是否在另一个服在线"""
        result = yield theseed.cross_server.query(
            realm="realm_b",
            method="Player.checkOnline",
            args={"uid": friend_uid}
        )
        if result.online:
            self.callClient("onFriendOnline", friend_uid)

    # === 场景 3: 跨服邮件/交易 ===
    def sendCrossServerMail(self, target_realm, target_uid, mail):
        """给另一个服的玩家发邮件"""
        theseed.cross_server.send_message(
            realm=target_realm,
            method="Mail.receive",
            args={"uid": target_uid, "mail": mail},
            guaranteed=True     # 保证送达（持久化消息）
        )

    # === 场景 4: 跨服活动排名 ===
    def getCrossServerRank(self):
        """获取跨服排行榜"""
        rank = yield theseed.cross_server.query(
            realm="activity_server",          # 活动专用服
            method="ActivityRank.getTop",
            args={"activity_id": 1001, "count": 100}
        )
        self.callClient("onRankData", rank)
```

### 7.2 跨服底层机制

```cpp
// crossserver/CrossServerManager.h

class CrossServerManager {
public:
    // 发起跨服调用
    Future<CrossResult> call(const std::string& realm,
                             const std::string& method,
                             const Payload& args,
                             CrossCallOptions options = {});

    // 跨服实体迁移
    Future<MigrationResult> migrateEntity(EntityId id,
                                          const std::string& targetRealm,
                                          const ServerEndpoint& target);

    // 跨服消息（fire-and-forget，可保证送达）
    void sendMessage(const std::string& realm,
                     const std::string& method,
                     const Payload& args,
                     bool guaranteed = false);

    // 跨服查询
    Future<QueryResult> query(const std::string& realm,
                              const std::string& method,
                              const Payload& args);

    // 服务发现（跨服）
    std::vector<RealmInfo> discoverRealms(const std::string& service);

private:
    // 底层使用 MessageBus
    std::shared_ptr<IMessageBus> bus_;

    // 消息桥接到其他 realm 的 NATS 集群
    std::shared_ptr<RealmBridge> bridge_;
};
```

### 7.3 Realm Bridge（域桥）

```
┌─────────────────────────────────────────────────────┐
│                   Realm Bridge                       │
│                                                      │
│  职责：                                              │
│  1. 消息格式转换（本域格式 → 跨域格式）               │
│  2. Trace Context 传播（OTel 跨域追踪）              │
│  3. 消息持久化（guaranteed 消息在桥上暂存）           │
│  4. 认证（跨域消息需要 realm 间认证）                 │
│  5. 流控（防止跨域消息淹没目标域）                    │
│                                                      │
│  实现：                                              │
│  - 同机房：NATS Leafnode（零配置集群互联）            │
│  - 跨机房：NATS Gateway 或自定义 TCP bridge           │
│                                                      │
│  配置示例：                                          │
│  bridges:                                            │
│    - realm: "realm_b"                                │
│      endpoint: "nats://realm-b-nats:4222"            │
│      auth:                                           │
│        method: token                                 │
│        token: "${REALM_B_BRIDGE_TOKEN}"              │
│      subjects: ["cross_realm.>"]                     │
│      max_rate: 10000           # 每秒最大跨域消息     │
└─────────────────────────────────────────────────────┘
```

---

## 8. 开箱即用的配置总览

```yaml
# config/theseed.yaml — 一个文件管所有

theseed:
  # 进程角色
  role: baseapp                       # loginapp / baseapp / cellapp / dbmgr / gateway

  # 集群
  cluster:
    name: "prod-cn-east"
    realm: "realm_a"

  # 网关（仅 gateway 角色生效）
  gateway:
    listeners:
      - protocol: tcp
        port: 20000
      - protocol: websocket
        port: 20001

  # 消息总线
  message_bus:
    backend: nats                      # nats / aeron / redis
    endpoints:
      - "nats://nats-1:4222"
      - "nats://nats-2:4222"
      - "nats://nats-3:4222"
    reconnect_wait: 2s
    max_reconnects: 60

  # Redis
  redis:
    host: "redis-master:6379"
    password: "${REDIS_PASSWORD}"
    pool:
      min: 4
      max: 32

  # 数据库
  database:
    backend: mysql                     # mysql / postgresql / mongodb
    host: "mysql-master:3306"
    database: "theseed_game"
    user: "theseed"
    password: "${DB_PASSWORD}"
    pool:
      min: 2
      max: 16

  # 可观测性
  observability:
    tracing:
      enabled: true
      endpoint: "otel-collector:4317"
      sampler: tail                    # head / tail / always_on / always_off
      sample_rate: 0.05                # 5% 采样率
    metrics:
      enabled: true
      endpoint: "otel-collector:4317"
      export_interval: 10s
    logging:
      enabled: true
      endpoint: "otel-collector:4317"
      level: info

  # 管理
  admin:
    http_port: 9090                    # 管理端口
    debug_endpoints: false             # 生产环境关闭 debug 接口

  # 跨服
  cross_server:
    enabled: true
    bridges:
      - realm: "realm_b"
        endpoint: "nats://realm-b-nats:4222"
        subjects: ["cross_realm.>"]
```

---

## 9. 目录结构

```
theseed/
├── src/
│   ├── gateway/                  # 网关层
│   │   ├── Gateway.h/cpp
│   │   ├── Router.h/cpp
│   │   ├── AuthHandler.h/cpp
│   │   ├── RateLimiter.h/cpp
│   │   └── TLSManager.h/cpp
│   │
│   ├── messagebus/               # 消息总线
│   │   ├── IMessageBus.h
│   │   ├── NATSBackend.h/cpp     # NATS 实现
│   │   ├── AeronBackend.h/cpp    # Aeron 实现（可选）
│   │   └── RedisBackend.h/cpp    # Redis Streams 实现（可选）
│   │
│   ├── cache/                    # 缓存层
│   │   ├── IRedisProvider.h
│   │   ├── RedisProvider.h/cpp   # 基于 hiredis/hiredis-cluster
│   │   ├── DistributedLock.h/cpp
│   │   └── Leaderboard.h/cpp
│   │
│   ├── async/                    # 异步框架
│   │   ├── Future.h              # Future/Promise
│   │   ├── AsyncExecutor.h/cpp
│   │   ├── TaskQueue.h/cpp
│   │   └── TimerScheduler.h/cpp
│   │
│   ├── crossserver/              # 跨服
│   │   ├── CrossServerManager.h/cpp
│   │   ├── RealmBridge.h/cpp
│   │   └── EntityProxy.h/cpp
│   │
│   └── logging/                  # 日志
│       ├── StructuredLog.h/cpp
│       └── FileRotation.h/cpp
│
├── config/
│   ├── theseed.yaml              # 主配置
│   ├── gateway.yaml              # 网关配置
│   ├── redis.yaml                # Redis 配置
│   └── cross_server.yaml         # 跨服配置
│
└── deployments/
    ├── nats/                     # NATS 部署配置
    │   └── nats-server.conf
    └── redis/                    # Redis 部署配置
        └── redis.conf
```

---

## 10. 与 KBEngine 的对比

| 基础设施 | KBEngine | theseed |
|---------|---------|---------|
| **网关** | LoginApp（简单 TCP 代理） | 内建 Gateway（TLS/限流/路由/WS） |
| **消息队列** | 无（直连 TCP） | NATS（发布/订阅/持久化/跨服桥接） |
| **Redis** | 有基础支持 | 完整封装（缓存/锁/排行榜/限流） |
| **异步** | CallbackMgr（回调） | Future/Promise + 脚本层 yield |
| **跨服** | 无 | Realm Bridge + 统一 API |
| **日志收集** | 文件日志 + 手动 grep | OTel Logs → Loki/Elasticsearch |
| **配置** | XML 散落 | 单一 YAML + 环境变量 |
| **开箱即用** | 需要大量外部配置 | 一个 YAML 启动 |
