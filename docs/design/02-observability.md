# Observability & Operations Design

> theseed 的可观测性与运维体系设计。
> 核心决策：**基于 OpenTelemetry（OTel）作为可观测性基础设施**，不造轮子。
> 目标：千台集群的全链路可观测，从一行代码的执行到跨服 EntityCall 的完整追踪。

---

## 1. 为什么选 OpenTelemetry

| 维度 | 自建方案 | OTel 方案 |
|------|---------|----------|
| 数据格式 | 自定义，每个 backend 重新对接 | OTLP 统一协议，所有 backend 通用 |
| 语言支持 | C++ 自建，脚本层需单独对接 | C++ / Python / Lua 都有官方 SDK |
| 生态 | 无 | Jaeger / Tempo / Grafana / Datadog / 阿里云 ARMS 全兼容 |
| 标准 | 无 | CNCF 毕业项目，云原生事实标准 |
| 维护成本 | 高 | 低，社区持续演进 |
| 采样策略 | 自建 | 尾部采样、概率采样、优先级采样都内建 |

**关键优势**：OTel 的 `Context Propagation` 机制天然支持 theseed 的跨进程 EntityCall——TraceID 和 SpanID 随消息自动传播，不需要在业务代码中手动传递。

---

## 2. 架构总览

```
┌─────────────────────────────────────────────────────────────────┐
│  Visualization Layer                                            │
│  ├─ Grafana Dashboards (Metrics + Traces + Logs 联动)           │
│  ├─ Alertmanager (告警规则、通知渠道、抑制/静默)                  │
│  └─ Custom theseed Dashboard (Entity 视角、Space 视角)           │
├─────────────────────────────────────────────────────────────────┤
│  Backend Layer (可替换，OTLP 协议统一)                           │
│  ├─ Traces:  Jaeger / Grafana Tempo / 阿里云 ARMS              │
│  ├─ Metrics: Prometheus / VictoriaMetrics                       │
│  └─ Logs:    Loki / Elasticsearch                               │
├─────────────────────────────────────────────────────────────────┤
│  Collection Layer                                               │
│  ├─ OTel Collector (中央收集，负责采样、处理、路由)               │
│  │   ├─ Receiver: OTLP gRPC/HTTP                                │
│  │   ├─ Processor: tail_sampling, batch, attributes             │
│  │   └─ Exporter: jaeger / prometheus / loki                    │
│  │                                                              │
│  └─ In-process OTel SDK (嵌入引擎进程)                           │
│      ├─ TracerProvider  → Span 创建与导出                       │
│      ├─ MeterProvider   → Counter / Histogram / Gauge           │
│      └─ LoggerProvider  → 结构化日志                            │
├─────────────────────────────────────────────────────────────────┤
│  Instrumentation Layer (埋点)                                    │
│  ├─ C++ Probe: 自动 span / metric / log                        │
│  ├─ Script Probe: Python cProfile / Lua profiler 对接           │
│  └─ System Probe: CPU / MEM / NET / FD / Disk                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## 3. Tracing 设计

### 3.1 Span 体系

游戏服务器的 trace 有天然的层级关系：

```
[Game Session Trace]                          ← 玩家一次完整会话
  └─ [Tick Span]                              ← 每个 tick 一个 span
       ├─ [Network Process Span]              ← 网络消息处理
       │    ├─ [Message: Player.attack]       ← 单条消息处理
       │    └─ [Message: Player.move]
       ├─ [Script Execute Span]               ← 脚本逻辑
       │    └─ [Python: Avatar.onAttack]
       ├─ [AOI Update Span]                   ← AOI 更新
       │    ├─ [Range Trigger Check]
       │    └─ [Witness Update]
       ├─ [Property Sync Span]                ← 属性同步
       └─ [Timer Callback Span]               ← 定时器回调

[EntityCall Trace]                            ← 跨进程 RPC
  ├─ [BaseApp_1] Player.onLevelUp
  │    └─ [Send EntityCall to CellApp_3]
  │         └─ [CellApp_3] Avatar.onLevelUp   ← TraceID 自动传播
  │              └─ [DB Write]                 ← 同一条 trace
  └─ [Return] (如果需要回调)
```

### 3.2 核心接口

```cpp
// observability/Tracing.h

#include <opentelemetry/trace/provider.h>
#include <opentelemetry/context/context.h>

namespace theseed::observability {

namespace trace = opentelemetry::trace;

class Tracing {
public:
    // 初始化
    static void init(const TracingConfig& config);

    // 获取 tracer
    static trace::Tracer* getTracer(const std::string& name = "theseed");

    // --- 便捷方法 ---

    // 创建 span（RAII，离开作用域自动结束）
    static ScopeSpan startSpan(const std::string& name,
                               const SpanAttributes& attrs = {});

    // 创建子 span
    static ScopeSpan startChildSpan(const std::string& name,
                                    const SpanAttributes& attrs = {});

    // EntityCall 跨进程时注入/提取 trace context
    static void injectContext(Bundle& bundle);  // 发送方：把 trace context 写入消息头
    static SpanContext extractContext(const Bundle& bundle);  // 接收方：从消息头恢复

    // 给当前 span 添加事件
    static void addEvent(const std::string& name,
                         const SpanAttributes& attrs = {});

    // 设置当前 span 状态
    static void setError(const std::string& message);

    // 标记当前 span 为重要（影响采样决策）
    static void setImportant();
};

// RAII span guard
class ScopeSpan {
public:
    ScopeSpan(trace::SpanPtr span) : span_(std::move(span)) {}
    ~ScopeSpan() { if (span_) span_->End(); }

    // 禁止拷贝
    ScopeSpan(const ScopeSpan&) = delete;
    ScopeSpan& operator=(const ScopeSpan&) = delete;

    trace::Span* operator->() { return span_.get(); }

private:
    trace::SpanPtr span_;
};

} // namespace theseed::observability
```

### 3.3 EntityCall 的 Trace Context 传播

这是最关键的集成点——EntityCall 跨进程时，trace context 必须随消息传播：

```cpp
// 在 ITransport 实现中集成
class TransportBase : public ITransport {
    void sendMessage(const Message& msg) override {
        // 1. 自动注入 trace context 到消息头
        auto span = Tracing::startSpan("entitycall.send", {
            {"entity.from", msg.fromEntity()},
            {"entity.to", msg.toEntity()},
            {"method", msg.methodName()},
            {"size", msg.payloadSize()}
        });

        // 2. 注入 trace context（W3C TraceContext 格式）
        Tracing::injectContext(msg.mutableBundle());

        // 3. 发送
        doSend(msg);

        // 4. span 在作用域结束时自动关闭
    }

    void onMessageReceived(const Message& msg) override {
        // 1. 从消息头提取 trace context，恢复分布式 trace
        auto parentCtx = Tracing::extractContext(msg.bundle());

        // 2. 创建子 span（parent 是发送方创建的 span）
        auto span = Tracing::startChildSpan("entitycall.recv", {
            {"entity.target", msg.targetEntity()},
            {"method", msg.methodName()}
        });

        // 3. 分发到处理器
        dispatchMessage(msg);
    }
};
```

### 3.4 关键 trace 点

```cpp
// 自动埋点的位置和 span 名称

// === 进程级别 ===
"process.tick"              // 每个 tick
"process.idle"              // tick 间空闲

// === Entity 生命周期 ===
"entity.create"             // Entity 创建
"entity.destroy"            // Entity 销毁
"entity.migrate"            // Entity 跨进程迁移
"entity.migrate.serialize"  // 迁移序列化
"entity.migrate.deserialize"// 迁移反序列化

// === 消息 / EntityCall ===
"entitycall.send"           // 发送 EntityCall
"entitycall.recv"           // 接收 EntityCall
"entitycall.execute"        // 执行 EntityCall 对应方法

// === 脚本 ===
"script.execute"            // 脚本方法执行
"script.timer"              // 定时器回调
"script.reload"             // 脚本热更

// === 网络 ===
"network.send"              // 网络发送
"network.recv"              // 网络接收
"network.connect"           // 新连接
"network.disconnect"        // 断开

// === AOI ===
"aoi.update"                // AOI 更新
"aoi.enter"                 // 实体进入视野
"aoi.leave"                 // 实体离开视野

// === 持久化 ===
"db.query"                  // 数据库查询
"db.save"                   // 数据库保存
"db.load"                   // 数据库加载
"db.migrate"                // Schema 迁移
```

---

## 4. Metrics 设计

### 4.1 指标分类

```
Metrics
├── System Metrics (系统指标)
│   ├── process_cpu_seconds          CPU 使用
│   ├── process_memory_bytes         内存使用
│   ├── process_open_fds             文件描述符
│   ├── process_uptime_seconds       运行时间
│   └── network_io_bytes             网络 I/O
│
├── Engine Metrics (引擎指标)
│   ├── theseed_tick_duration_ms     tick 耗时 (Histogram)
│   ├── theseed_tick_count           tick 计数 (Counter)
│   ├── theseed_entity_count         实体数 (Gauge)
│   ├── theseed_entity_created       实体创建 (Counter)
│   ├── theseed_entity_destroyed     实体销毁 (Counter)
│   ├── theseed_message_in_count     入消息数 (Counter)
│   ├── theseed_message_out_count    出消息数 (Counter)
│   ├── theseed_message_in_bytes     入消息字节 (Counter)
│   ├── theseed_message_out_bytes    出消息字节 (Counter)
│   └── theseed_bundle_pool_size     Bundle 对象池大小 (Gauge)
│
├── AOI Metrics (空间指标)
│   ├── theseed_aoi_entity_count     AOI 内实体数 (Gauge)
│   ├── theseed_aoi_update_ms        AOI 更新耗时 (Histogram)
│   ├── theseed_aoi_enter_count      进入视野 (Counter)
│   └── theseed_aoi_leave_count      离开视野 (Counter)
│
├── Script Metrics (脚本指标)
│   ├── theseed_script_exec_ms       脚本执行耗时 (Histogram)
│   ├── theseed_script_error_count   脚本错误数 (Counter)
│   └── theseed_script_gc_ms         脚本 GC 耗时 (Histogram)
│
├── Storage Metrics (存储指标)
│   ├── theseed_db_query_ms          数据库查询耗时 (Histogram)
│   ├── theseed_db_query_count       查询计数 (Counter)
│   ├── theseed_db_error_count       错误计数 (Counter)
│   ├── theseed_db_connection_count  连接数 (Gauge)
│   └── theseed_db_pool_wait_ms      连接池等待 (Histogram)
│
└── Business Metrics (业务指标，脚本可自定义)
    ├── game_online_player_count     在线玩家 (Gauge)
    ├── game_match_duration_ms       匹配耗时 (Histogram)
    └── game_battle_count            战斗数 (Counter)
```

### 4.2 核心接口

```cpp
// observability/Metrics.h

#include <opentelemetry/metrics/provider.h>

namespace theseed::observability {

namespace metrics = opentelemetry::metrics;

class Metrics {
public:
    static void init(const MetricsConfig& config);

    // --- 便捷方法 ---

    // Counter: 单调递增计数
    static void counter(const std::string& name, double value = 1.0,
                        const Attributes& attrs = {});

    // Histogram: 值分布（延迟、大小等）
    static void histogram(const std::string& name, double value,
                          const Attributes& attrs = {});

    // Gauge: 当前值（实体数、连接数等）
    static void gauge(const std::string& name, double value,
                      const Attributes& attrs = {});

    // UpDownCounter: 可增可减（队列长度等）
    static void upDownCounter(const std::string& name, double value = 1.0,
                              const Attributes& attrs = {});
};

// RAII 计时器，自动记录到 histogram
class ScopeTimer {
public:
    ScopeTimer(const std::string& metric_name, const Attributes& attrs = {})
        : name_(metric_name), attrs_(attrs), start_(std::chrono::steady_clock::now()) {}

    ~ScopeTimer() {
        auto elapsed = std::chrono::steady_clock::now() - start_;
        auto ms = std::chrono::duration<double, std::milli>(elapsed).count();
        Metrics::histogram(name_, ms, attrs_);
    }

private:
    std::string name_;
    Attributes attrs_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace theseed::observability
```

### 4.3 编译期消解

```cpp
// observability/Config.h

#ifdef THESEED_ENABLE_OBSERVABILITY
    #define TRACE_SPAN(name, ...) theseed::observability::Tracing::startSpan(name, ##__VA_ARGS__)
    #define TRACE_CHILD(name, ...) theseed::observability::Tracing::startChildSpan(name, ##__VA_ARGS__)
    #define TRACE_EVENT(name, ...) theseed::observability::Tracing::addEvent(name, ##__VA_ARGS__)
    #define TRACE_ERROR(msg) theseed::observability::Tracing::setError(msg)

    #define METRIC_COUNTER(name, ...) theseed::observability::Metrics::counter(name, ##__VA_ARGS__)
    #define METRIC_HISTOGRAM(name, ...) theseed::observability::Metrics::histogram(name, ##__VA_ARGS__)
    #define METRIC_GAUGE(name, ...) theseed::observability::Metrics::gauge(name, ##__VA_ARGS__)

    #define PROFILE_SCOPE(name) theseed::observability::ScopeTimer scope_timer_##__LINE__(name)
#else
    #define TRACE_SPAN(name, ...) ((void)0)
    #define TRACE_CHILD(name, ...) ((void)0)
    #define TRACE_EVENT(name, ...) ((void)0)
    #define TRACE_ERROR(msg) ((void)0)

    #define METRIC_COUNTER(name, ...) ((void)0)
    #define METRIC_HISTOGRAM(name, ...) ((void)0)
    #define METRIC_GAUGE(name, ...) ((void)0)

    #define PROFILE_SCOPE(name) ((void)0)
#endif
```

### 4.4 脚本层 Metrics 暴露

```python
# Python 脚本中可以直接使用
import theseed

# 业务指标
theseed.metrics.counter("game.battle.count", attrs={"type": "pvp"})
theseed.metrics.histogram("game.match.duration_ms", 1250.0, attrs={"mode": "ranked"})
theseed.metrics.gauge("game.online.players", 15234)
```

```lua
-- Lua 脚本中同样支持
theseed.metrics.counter("game.battle.count", {type = "pvp"})
theseed.metrics.histogram("game.match.duration_ms", 1250.0, {mode = "ranked"})
theseed.metrics.gauge("game.online.players", 15234)
```

---

## 5. Logging 设计

### 5.1 结构化日志

```cpp
// observability/Logging.h

#include <opentelemetry/logs/logger.h>

namespace theseed::observability {

class Log {
public:
    static void init(const LoggingConfig& config);

    // 结构化日志
    static void emit(LogLevel level,
                     const std::string& message,
                     const Attributes& attrs = {});

    // 便捷宏
    #define LOG_DEBUG(msg, ...) Log::emit(LogLevel::Debug, msg, ##__VA_ARGS__)
    #define LOG_INFO(msg, ...)  Log::emit(LogLevel::Info, msg, ##__VA_ARGS__)
    #define LOG_WARN(msg, ...)  Log::emit(LogLevel::Warn, msg, ##__VA_ARGS__)
    #define LOG_ERROR(msg, ...) Log::emit(LogLevel::Error, msg, ##__VA_ARGS__)
    #define LOG_FATAL(msg, ...) Log::emit(LogLevel::Fatal, msg, ##__VA_ARGS__)
};

} // namespace theseed::observability
```

### 5.2 日志格式

```json
{
  "timestamp": "2026-05-19T14:23:45.123456789Z",
  "severity": "INFO",
  "body": "Entity created",
  "resource": {
    "service.name": "theseed-cellapp",
    "service.instance.id": "cellapp-003",
    "process.pid": 12345,
    "host.name": "game-server-042"
  },
  "attributes": {
    "entity.id": 100042,
    "entity.type": "Avatar",
    "entity.side": "cell",
    "space.id": 5001,
    "trace_id": "abc123def456",
    "span_id": "789ghi012"
  }
}
```

### 5.3 日志分级策略

```
生产环境默认级别: INFO
Debug 模式: DEBUG
性能敏感路径: WARN 以上

额外规则:
- Entity 创建/销毁: INFO（可审计）
- 消息收发: DEBUG（生产环境关闭，排查时临时开启）
- AOI 更新: DEBUG（高频，默认关闭）
- 脚本执行: DEBUG（默认关闭，error 时自动提升为 ERROR）
- 错误: ERROR（始终记录）
- 致命错误: FATAL（记录后触发告警）
```

---

## 6. 采样策略

### 6.1 问题

游戏服务器每秒产生海量 trace 数据（每个 tick 数十到数百 span），千台集群的量级是 **每秒百万级 span**。不能全量采集。

### 6.2 分层采样

```
OTel Collector
├── 尾部采样 (Tail Sampling Processor)
│   ├── 规则 1: ERROR / FATAL 的 trace → 100% 保留
│   ├── 规则 2: 耗时 > 阈值的 trace → 100% 保留
│   ├── 规则 3: 标记为 important 的 trace → 100% 保留
│   ├── 规则 4: Entity 迁移的 trace → 100% 保留
│   ├── 规则 5: 普通 tick trace → 1% 概率采样
│   └── 规则 6: 脚本执行 trace → 5% 概率采样
│
└── 批量处理 (Batch Processor)
    ├── 发送间隔: 5s
    └── 批量大小: 1024 spans
```

### 6.3 theseed 的 span 标记规则

```cpp
// 在引擎内部自动标记重要 span
void Entity::onError(const std::string& msg) {
    // 错误 span 自动标记为 important
    Tracing::setError(msg);
    Tracing::setImportant();  // 影响采样：这条 trace 会被保留
}

void Entity::migrate(const ServerEndpoint& target) {
    // 迁移 span 自动标记
    auto span = Tracing::startSpan("entity.migrate", {
        {"entity.id", id_},
        {"target.host", target.host},
        {"target.port", target.port}
    });
    Tracing::setImportant();  // 迁移 trace 必须保留
    // ...
}

// 慢 tick 自动标记
void Process::endTick(const TickProfile& profile) {
    if (profile.total_ms > config_.tick_budget_ms * 2.0) {
        Tracing::addEvent("tick.slow", {{"duration_ms", profile.total_ms}});
        Tracing::setImportant();
    }
}
```

---

## 7. 告警体系

### 7.1 告警规则定义

```yaml
# config/alerts.yaml

alerts:
  # === 进程健康 ===
  - name: process_down
    type: absence                          # 心跳丢失
    metric: theseed_process_heartbeat
    window: 30s
    severity: critical
    channels: [pagerduty, slack]

  - name: process_restart
    type: change                           # 进程重启
    metric: theseed_process_uptime_seconds
    condition: "decreased"
    severity: warning
    channels: [slack]

  # === 性能 ===
  - name: tick_slow
    type: threshold
    metric: theseed_tick_duration_ms
    condition: "p99 > 200"
    window: 60s
    severity: warning
    channels: [slack]

  - name: tick_stall
    type: threshold
    metric: theseed_tick_duration_ms
    condition: "p99 > 1000"
    window: 30s
    severity: critical
    channels: [pagerduty, slack]

  # === 资源 ===
  - name: memory_high
    type: threshold
    metric: process_memory_bytes
    condition: "value > 0.85 * limit"
    window: 120s
    severity: warning

  - name: fd_exhaustion
    type: threshold
    metric: process_open_fds
    condition: "value > 0.8 * limit"
    window: 60s
    severity: critical

  # === 业务 ===
  - name: entity_leak
    type: threshold
    metric: theseed_entity_count
    condition: "trend(increase) > 100/min sustained 10min"
    severity: warning
    channels: [slack]

  - name: db_slow
    type: threshold
    metric: theseed_db_query_ms
    condition: "p99 > 500"
    window: 120s
    severity: warning

  # === 自定义（脚本层） ===
  - name: match_timeout
    type: threshold
    metric: game_match_duration_ms
    condition: "p99 > 30000"
    window: 300s
    severity: warning
    channels: [slack]
```

### 7.2 告警通知渠道

```
Alertmanager
├── PagerDuty    → 值班手机告警 (critical)
├── Slack/飞书   → 团队通知 (warning + critical)
├── Email        → 日报/周报汇总
├── Webhook      → 自定义处理（自动扩容、自动重启）
└── 这些都由 OTel → Alertmanager 集成，不自建
```

---

## 8. Dashboard 设计

### 8.1 预置 Dashboard

```
Grafana Dashboards
├── Overview
│   ├── 集群总览（所有进程状态一览）
│   ├── 在线玩家趋势
│   └── 关键指标健康度
│
├── Process Detail
│   ├── Tick 耗时热力图 (per process)
│   ├── 消息吞吐 (in/out)
│   ├── 实体水位
│   └── 内存/CPU 趋势
│
├── Entity View
│   ├── 实体创建/销毁速率
│   ├── 按类型分布
│   ├── EntityCall 延迟分布
│   └── 迁移频率与耗时
│
├── Network View
│   ├── 连接数趋势
│   ├── 带宽使用
│   ├── 消息大小分布
│   └── 错误率
│
├── AOI View
│   ├── AOI 实体密度热力图
│   ├── 进入/离开视野频率
│   └── Witness 更新耗时
│
├── Storage View
│   ├── 查询延迟分布
│   ├── 慢查询列表
│   ├── 连接池水位
│   └── 读写 QPS
│
├── Script View
│   ├── 脚本执行耗时 Top N
│   ├── 脚本错误率
│   └── GC 耗时
│
└── Traces Explorer
    ├── 搜索 trace（按 entity / method / duration）
    ├── trace 瀑布图
    └── 关联 logs（trace_id 联动）
```

### 8.2 Metrics → Traces → Logs 联动

这是 OTel 最强大的能力——三层联动：

```
用户操作流程：
1. 在 Grafana Dashboard 看到 tick 耗时异常飙升
2. 点击对应时间段的 spike → 跳转到 Traces Explorer
3. 找到慢 tick 对应的 trace → 查看瀑布图
4. 发现是某个 EntityCall 到 DB 的查询特别慢
5. 点击该 span → 跳转到 Logs（通过 trace_id 关联）
6. 看到具体的 DB 错误日志："connection pool exhausted"
7. 根因定位完成

整个过程不需要登录任何服务器，不需要 grep 任何日志。
```

---

## 9. 运维操作集成

### 9.1 健康检查

```cpp
// ops/HealthCheck.h

struct HealthStatus {
    enum State { Healthy, Degraded, Unhealthy };
    State state;
    std::string message;
    json details;
};

class IHealthCheck {
public:
    virtual ~IHealthCheck() = default;

    // 基础健康
    virtual HealthStatus checkProcess() = 0;

    // 组件健康
    virtual HealthStatus checkNetwork() = 0;
    virtual HealthStatus checkDatabase() = 0;
    virtual HealthStatus checkScript() = 0;

    // 水位
    virtual HealthStatus checkEntityCapacity() = 0;
    virtual HealthStatus checkMemoryCapacity() = 0;

    // 综合
    virtual HealthStatus overall() = 0;
};
```

### 9.2 HTTP 管理端口

每个引擎进程暴露一个轻量 HTTP 管理端口（不走游戏网络）：

```
:9090/metrics          → Prometheus metrics (OTel Prometheus exporter)
:9090/health           → 健康检查 JSON
:9090/ready            → K8s readiness probe
:9090/live             → K8s liveness probe
:9090/debug/pprof      → Go-style pprof (CPU/MEM profile)
:9090/debug/vars       → 运行时变量
:9090/debug/entity/:id → 单个实体详情（开发模式）
:9090/debug/entities   → 实体列表摘要（开发模式）
```

### 9.3 自动扩缩容

```yaml
# config/autoscaling.yaml

autoscaling:
  cellapp:
    min: 4
    max: 50
    metrics:
      - type: resource
        metric: theseed_entity_count
        target_per_instance: 5000
      - type: resource
        metric: theseed_tick_duration_ms_p99
        target: 100

    scale_up:
      cooldown: 60s
      steps: 2              # 每次最多扩 2 个
    scale_down:
      cooldown: 300s
      steps: 1              # 每次最多缩 1 个
      drain_timeout: 120s   # 缩容前等待实体迁移

  baseapp:
    min: 2
    max: 20
    metrics:
      - type: resource
        metric: theseed_entity_count
        target_per_instance: 10000
```

---

## 10. 与 KBEngine 的完整对比

| 能力 | KBEngine | theseed (OTel) |
|------|---------|---------------|
| **Tracing** | 无 | OpenTelemetry，跨进程 EntityCall 自动传播 |
| **Metrics** | Watcher（有限、手动） | OTel Metrics，Counter/Histogram/Gauge |
| **Logging** | `LOG_MSG` 宏，文本日志 | OTel Logs，结构化 JSON |
| **分布式追踪** | 无 | Jaeger/Tempo，全链路追踪 |
| **Dashboard** | 无内建 | Grafana 预置 Dashboard |
| **告警** | 无内建 | Alertmanager，多渠道通知 |
| **健康检查** | Machine 心跳 | HTTP /health + K8s probe |
| **性能分析** | 手动 Profiler | pprof + OTel Metrics |
| **日志与 Trace 关联** | 无 | trace_id 贯穿 logs/metrics/traces |
| **采样** | 无 | 尾部采样，自动保留错误和慢请求 |
| **脚本层** | Python print | OTel SDK 集成，脚本可发 metric/log |
| **标准** | 私有 | OTLP (CNCF 标准) |

---

## 11. 目录结构

```
theseed/
├── src/
│   ├── observability/                  # 可观测性模块
│   │   ├── CMakeLists.txt
│   │   ├── Config.h                    # 编译期开关宏
│   │   ├── Tracing.h/cpp              # OTel Tracing 封装
│   │   ├── Metrics.h/cpp              # OTel Metrics 封装
│   │   ├── Logging.h/cpp              # OTel Logs 封装
│   │   ├── ScopeTimer.h               # RAII 计时器
│   │   ├── ScopeSpan.h                # RAII span
│   │   ├── ContextPropagation.h/cpp   # EntityCall trace context 传播
│   │   └── init.cpp                   # OTel SDK 初始化
│   │
│   ├── ops/                            # 运维模块
│   │   ├── CMakeLists.txt
│   │   ├── HealthCheck.h/cpp          # 健康检查
│   │   ├── AdminServer.h/cpp          # HTTP 管理端口
│   │   ├── ProcessSupervisor.h/cpp    # 进程守护
│   │   └── ConfigSync.h/cpp           # 配置热同步
│   │
│   └── integration/                    # 集成埋点
│       ├── EntityTracing.cpp          # Entity 生命周期 trace
│       ├── TransportTracing.cpp       # 网络层 trace + context 传播
│       ├── ScriptTracing.cpp          # 脚本执行 trace
│       ├── StorageTracing.cpp         # 存储层 trace
│       └── AOITracing.cpp             # AOI 操作 trace
│
├── config/
│   ├── observability.yaml              # 可观测性配置
│   ├── alerts.yaml                     # 告警规则
│   └── autoscaling.yaml               # 自动扩缩容
│
├── deployments/
│   ├── otel-collector/
│   │   └── config.yaml                 # OTel Collector 配置
│   ├── grafana/
│   │   └── dashboards/                 # 预置 Dashboard JSON
│   └── alertmanager/
│       └── config.yaml                 # Alertmanager 配置
│
└── docs/
    └── design/
        └── 02-observability.md         # 本文档
```

---

## 12. 初始化顺序

```cpp
// main.cpp 中的初始化顺序

int main() {
    // 1. 解析命令行和配置
    auto config = Config::load("config/server.yaml");

    // 2. 初始化可观测性（最先初始化，后续所有操作都能被观测）
    theseed::observability::Tracing::init(config.tracing);
    theseed::observability::Metrics::init(config.metrics);
    theseed::observability::Logging::init(config.logging);

    // 3. 启动 HTTP 管理端口
    theseed::ops::AdminServer::start(config.admin);

    // 4. 初始化核心层
    auto transport = createTransport(config.transport);
    auto storage   = createStorage(config.storage);
    auto script    = createScriptBackend(config.script);

    // 5. 初始化引擎
    auto engine = Engine::create(transport, storage, script, config.engine);

    // 6. 启动主循环
    engine->run();  // 内部每个 tick 自动埋点
}
```

---

## 13. 性能开销评估

| 操作 | 开销 | 说明 |
|------|------|------|
| `TRACE_SPAN` (不采样) | ~50ns | 仅创建 noop span |
| `TRACE_SPAN` (采样) | ~200ns | 创建真实 span + 属性设置 |
| `METRIC_COUNTER` | ~100ns | atomic increment |
| `METRIC_HISTOGRAM` | ~150ns | 写入 histogram bucket |
| `LOG_INFO` (不采集) | ~30ns | level check 后跳过 |
| `LOG_INFO` (采集) | ~500ns | 格式化 + 写入 |
| Context Propagation | ~200ns | 读/写 2 个 header (trace_id + span_id) |
| OTel Export (batch) | ~0 | 后台线程异步，不影响主线程 |

**关键**：Release 构建可以选择 `THESEED_ENABLE_OBSERVABILITY=OFF`，所有开销归零。
游戏逻辑主线程上没有 I/O 操作——所有 export 都是异步的。
```
总体影响：< 1% tick time（典型 tick 10ms，可观测性开销 < 0.1ms）
```
