# Redis, Async & Config — Cache, Async, and Configuration

> English | **中文**

> theseed's out-of-the-box infrastructure components.
> Origin judgment: BigWorld has no modern Redis / Async / Config stack, and KBEngine only has basic Redis support.
> theseed does modern infrastructure extension at this layer.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
The BigWorld era had no complete infrastructure stack of today's "Redis + async framework + unified configuration center".

It was more like:
  - in-process configuration / watcher parameters
  - custom async over the database and script callbacks
  - a small amount of auxiliary storage and tooling
```

### How KBEngine Implements It

```
KBEngine has basic Redis support, but with a narrower positioning.

It leans more toward:
  - session / cache / simple distributed mutex
  - callback-based async
  - configuration scattered across scripts and XML
```

### Pros and Cons

```
BigWorld / KBEngine's advantages:
  - few dependencies
  - simple main path
  - shallow coupling with the runtime

BigWorld / KBEngine's drawbacks:
  - cache, lock, leaderboard, rate limiting, and async tasks have no unified governance
  - configuration and task models are scattered
  - lack of modern observable infrastructure wrapping
```

### theseed's Trade-offs

```
theseed chooses to put Redis, async tasks, and the configuration center into the infrastructure layer,
because these capabilities are consumed simultaneously by the runtime, ops, and script layers.

The cost is:
  - introducing additional external components
  - requiring a clear main-path boundary
  - requiring unified configuration and permission governance
```

---

## 1. Redis Integration

### 1.1 Uses

```
Redis's role in theseed
├── cache layer: entity hot data, online status, configuration cache
├── distributed lock: cross-process mutex, distributed timers, migration locks
├── leaderboard: Sorted Set real-time ranking
├── pub/sub: lightweight inter-process notification
├── rate limiting: token bucket / sliding window
└── session storage: login token, reconnection session
```

### 1.2 Core Interfaces

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

### 1.3 Script Layer

```python
import theseed

theseed.redis.set("player:10042:last_login", "2026-05-19", ttl=3600)
theseed.redis.zadd("leaderboard:daily_kill", "player:10042", 128)

with theseed.redis.lock("guild_war:match", ttl=30) as lock:
    if lock.acquired:
        do_match()
```

---

## 2. Async Task Framework

### 2.1 C++ Async Model

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
Thread constraints:
  - background threads may perform I/O
  - completion results must be delivered back to the owning tick thread
  - background threads must not modify Entities directly
  - the tick thread must not block waiting on Future.get()
```

### 2.2 Script Layer Async

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
Script constraints:
  - yield resumption points must return to the tick thread owning the entity
  - before async resumption, entity epoch / liveness state must be verified
  - late callbacks after migration may be dropped or retried, but must not write to the old entity directly
```

### 2.3 Background Task Queues

```cpp
// 预定义队列
auto dbQueue = TaskQueue::get("db");
auto ioQueue = TaskQueue::get("io");
auto computeQueue = TaskQueue::get("compute");
```

---

## 3. Log Collection

```
Engine process
  ├─ OTel SDK → OTLP Exporter → OTel Collector
  └─ file log (fallback) → Fluent Bit Agent
                                       ↓
                              Loki / Elasticsearch
                                       ↓
                                   Grafana
```

---

## 4. Out-of-the-Box Configuration Overview

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

  telemetry:
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

## 5. Comparison with KBEngine

| Infrastructure | KBEngine | theseed |
|---------|---------|---------|
| Gateway | LoginApp (simple TCP) | built-in Gateway (TLS / rate limiting / routing / WS) |
| Message queue | none (direct TCP) | Runtime Transport + NATS |
| Redis | basic support | complete wrapper (cache / lock / leaderboard / rate limiting) |
| Async | CallbackMgr (callbacks) | Future/Promise + yield |
| Cross-realm | none | Realm Bridge + unified API |
| Log collection | file logs + grep | OTel → Loki/Elasticsearch |
| Configuration | scattered XML | single YAML + environment variables |
