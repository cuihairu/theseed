# Redis, Async & Config — 缓存、异步与配置

> theseed 基础设施的开箱即用组件。
> 来源：KBEngine 有基础 Redis 支持，theseed 大幅增强。

---

## 1. Redis 集成

### 1.1 用途

```
Redis 在 theseed 中的角色
├── 缓存层：实体热数据、在线状态、配置缓存
├── 分布式锁：跨进程互斥、分布式定时器、迁移锁
├── 排行榜：Sorted Set 实时排行
├── 发布/订阅：轻量级进程间通知
├── 限流：令牌桶/滑动窗口
└── 会话存储：登录 Token、断线重连 session
```

### 1.2 核心接口

```cpp
class IRedisProvider {
public:
    // 基础操作
    virtual Future<std::string> get(const std::string& key) = 0;
    virtual Future<void> set(const std::string& key, const std::string& value,
                             Duration ttl = Duration::zero()) = 0;

    // Sorted Set（排行榜）
    virtual Future<void> zadd(const std::string& key,
                              const std::string& member, double score) = 0;
    virtual Future<std::vector<RankEntry>> zrange(...) = 0;

    // 分布式锁
    virtual Future<LockHandle> lock(const std::string& key, Duration ttl, ...) = 0;

    // Pipeline / Lua 脚本
    virtual Future<std::vector<RedisReply>> pipeline(...) = 0;
    virtual Future<RedisReply> eval(const std::string& script, ...) = 0;
};
```

### 1.3 脚本层

```python
import theseed

theseed.redis.set("player:10042:last_login", "2026-05-19", ttl=3600)
theseed.redis.zadd("leaderboard:daily_kill", "player:10042", 128)

with theseed.redis.lock("guild_war:match", ttl=30) as lock:
    if lock.acquired:
        do_match()
```

---

## 2. 异步任务框架

### 2.1 C++ 异步模型

```cpp
template<typename T>
class Future {
public:
    template<typename Func>
    Future<std::invoke_result_t<Func, T>> then(Func&& f);
    Future<T> onError(std::function<T(const Error&)> handler);
    Future<T> timeout(Duration d);
    T get(Duration timeout = Duration::max());
};

// 典型使用
Future<PlayerData> loadPlayer(EntityId id) {
    return storage_->load(id, PlayerDef{})
        .timeout(3s)
        .then([](PlayerData data) { return processPlayer(data); })
        .onError([](const Error& e) { return PlayerData::default(); });
}
```

```
线程约束：
  - 后台线程可以执行 I/O
  - 完成结果必须投递回 owning tick thread
  - 不允许后台线程直接修改 Entity
  - 不允许 tick 线程阻塞等待 Future.get()
```

### 2.2 脚本层异步

```python
import theseed

class Avatar(theseed.BaseEntity):
    def onLogin(self):
        data = yield theseed.async_.db_load("Avatar", self.id)
        result = yield theseed.async_.cross_server_query("realm_b", ...)
        payment = yield theseed.async_.http_post("https://pay.example.com/verify", ...)

    @theseed.timer(interval=30)
    def onSave(self):
        yield theseed.async_.db_save(self)
```

```
脚本约束：
  - yield 恢复点必须回到实体所属 tick 线程
  - 异步恢复前必须校验 entity epoch / 存活状态
  - 迁移后迟到回调可以丢弃或重试，但不能直接写旧实体
```

### 2.3 后台任务队列

```cpp
// 预定义队列
auto dbQueue = TaskQueue::get("db");
auto ioQueue = TaskQueue::get("io");
auto computeQueue = TaskQueue::get("compute");
```

---

## 3. 日志收集

```
引擎进程
  ├─ OTel SDK → OTLP Exporter → OTel Collector
  └─ 文件日志（fallback）→ Fluent Bit Agent
                                       ↓
                              Loki / Elasticsearch
                                       ↓
                                   Grafana
```

---

## 4. 开箱即用配置总览

```yaml
theseed:
  role: baseapp

  cluster:
    name: "prod-cn-east"
    realm: "realm_a"

  gateway:
    listeners:
      - protocol: tcp
        port: 20000
      - protocol: websocket
        port: 20001

  runtime_transport:
    backend: aeron
    endpoint: "127.0.0.1:30123"
    use_ipc: true

  message_bus:
    # 只承载 Control Plane / Cross-Realm Async Plane
    backend: nats
    endpoints:
      - "nats://nats-1:4222"

  redis:
    host: "redis-master:6379"
    pool: { min: 4, max: 32 }

  database:
    backend: mysql
    host: "mysql-master:3306"
    database: "theseed_game"

  observability:
    tracing: { enabled: true, endpoint: "otel-collector:4317" }
    metrics: { enabled: true, export_interval: 10s }
    logging: { enabled: true, level: info }

  cross_server:
    enabled: true
    bridges:
      - realm: "realm_b"
        endpoint: "nats://realm-b-nats:4222"
```

---

## 5. 与 KBEngine 的对比

| 基础设施 | KBEngine | theseed |
|---------|---------|---------|
| 网关 | LoginApp（简单 TCP） | 内建 Gateway（TLS/限流/路由/WS） |
| 消息队列 | 无（直连 TCP） | Runtime Transport + NATS |
| Redis | 基础支持 | 完整封装（缓存/锁/排行/限流） |
| 异步 | CallbackMgr（回调） | Future/Promise + yield |
| 跨服 | 无 | Realm Bridge + 统一 API |
| 日志收集 | 文件日志 + grep | OTel → Loki/Elasticsearch |
| 配置 | XML 散落 | 单一 YAML + 环境变量 |
