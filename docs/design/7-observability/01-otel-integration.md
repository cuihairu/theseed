# Observability — 遥测、调试与 Profiling

> 基于开源遥测作为可观测性基础设施，不造轮子。
> 千台集群的全链路可观测，从代码执行到跨服 EntityCall 的完整追踪。
>
> 来源：KBEngine Watcher（有限手动），theseed 全面基于 OTel。
> 本篇只讨论 Telemetry / Debug / Profiling，不展开运维控制面；后者见 [02-ops-control-plane](02-ops-control-plane.md)。

---

## 0. 边界

```
本篇负责：
  - traces
  - metrics
  - logs
  - debug hooks
  - profiling

本篇不负责：
  - 分布式状态树查询
  - 在线命令执行
  - challenge / ban / shutdown 运维入口
  - 配置热改审计
```

---

## 1. 为什么选 OTel

```
关键优势：
  - OTLP 统一协议，所有 backend 通用
  - C++ / Python / Lua 都有官方 SDK
  - Context Propagation 天然支持 EntityCall 跨进程追踪
  - CNCF 毕业项目，云原生事实标准
```

---

## 2. 架构总览

```
Visualization: Grafana Dashboards + Alertmanager
     ↑
Backend: Jaeger/Tempo (Traces) + Prometheus (Metrics) + Loki (Logs)
     ↑
Collection: OTel Collector (采样、处理、路由)
     ↑
Instrumentation: C++ Probe + Script Probe + System Probe
```

---

## 3. Tracing

### 3.1 Span 体系

```
[Game Session Trace]
  └─ [Tick Span]
       ├─ [Network Process Span]
       │    ├─ [Message: Player.attack]
       │    └─ [Message: Player.move]
       ├─ [Script Execute Span]
       ├─ [AOI Update Span]
       └─ [Property Sync Span]

[EntityCall Trace]                   ← 跨进程
  ├─ [BaseApp_1] Player.onLevelUp
  │    └─ [CellApp_3] Avatar.onLevelUp   ← TraceID 自动传播
  └─ [DB Write]                           ← 同一条 trace
```

### 3.2 EntityCall Context Propagation

```cpp
// 发送方：注入 trace context
Tracing::injectContext(msg.mutableBundle());

// 接收方：恢复分布式 trace
auto parentCtx = Tracing::extractContext(msg.bundle());
auto span = Tracing::startChildSpan("entitycall.recv", ...);
```

### 3.3 关键 trace 点

```
process.tick, entity.create, entity.destroy, entity.migrate
entitycall.send, entitycall.recv, entitycall.execute
script.execute, script.timer, script.reload
aoi.update, aoi.enter, aoi.leave
db.query, db.save, db.load
```

---

## 4. Metrics

### 4.1 指标分类

```
Engine:   tick_duration_ms (Histogram), entity_count (Gauge), message_in/out (Counter)
AOI:      update_ms (Histogram), enter/leave_count (Counter)
Script:   exec_ms (Histogram), error_count (Counter), gc_ms (Histogram)
Storage:  query_ms (Histogram), connection_count (Gauge)
Business: online_player_count (Gauge), match_duration_ms (Histogram)
```

### 4.2 编译期消解

```cpp
#ifdef THESEED_ENABLE_OBSERVABILITY
    #define TRACE_SPAN(name, ...) theseed::observability::Tracing::startSpan(name, ##__VA_ARGS__)
    #define METRIC_COUNTER(name, ...) theseed::observability::Metrics::counter(name, ##__VA_ARGS__)
#else
    #define TRACE_SPAN(name, ...) ((void)0)
    #define METRIC_COUNTER(name, ...) ((void)0)
#endif
```

---

## 5. 采样策略

```
OTel Collector 尾部采样：
  - ERROR / FATAL trace → 100% 保留
  - 慢 tick trace → 100% 保留
  - Entity 迁移 trace → 100% 保留
  - 普通 tick trace → 1% 概率采样
  - 脚本执行 trace → 5% 概率采样
```

---

## 6. Debug 体系

### 6.1 架构

```
IDE Integration (VS Code / JetBrains)  ← DAP 协议
  ↓
Debug Bridge (统一接入层)
  ↓
Language Debug Adapter (C++ → LLDB, Python → debugpy, Lua → custom)
  ↓
Engine Debug Hooks (Entity/Message/Timer/AOI 断点)
```

### 6.2 核心接口

```cpp
class IDebugProvider {
    virtual void onEntityCreated(EntityId id, const std::string& entityType) = 0;
    virtual void onEntityDestroyed(EntityId id) = 0;
    virtual void onMessageSent(EntityId from, EntityId to, const std::string& method) = 0;
    virtual std::string inspectEntity(EntityId id) = 0;
    virtual void addConditionalBreakpoint(const std::string& condition, ...) = 0;
};
```

---

## 7. Profiling

> 这里的 Profiling 只讨论 Telemetry / flamegraph / slow tick 诊断。
> BigWorld 式 `EntityProfiler / EntityTypeProfiler → loadBalance / overload gate` 反馈链不在本篇，见 [../3-infrastructure/06-runtime-profiler-and-load-feedback](../3-infrastructure/06-runtime-profiler-and-load-feedback.md)。

### 7.1 设计

```
零成本抽象：release 构建下所有 probe 编译为空操作
tick 级粒度：回答"这个 tick 花了多少时间在哪个阶段"
火焰图就绪：采样数据直接输出 folded stack 格式
```

### 7.2 自动阈值告警

```cpp
void checkTickHealth(const TickProfile& p) {
    if (p.total_ms > g_config.tick_budget_ms * 0.8) {
        g_alerts->warn("tick slow", p);
    }
    if (p.script_ms > p.total_ms * 0.7) {
        g_alerts->warn("script heavy", p);
    }
}
```

---

## 8. 告警与自动扩缩

```yaml
alerts:
  - name: tick_stall
    condition: "p99 > 1000"
    severity: critical
  - name: entity_leak
    condition: "trend(increase) > 100/min sustained 10min"

autoscaling:
  cellapp:
    min: 4, max: 50
    metrics:
      - theseed_entity_count target_per_instance: 5000
```

---

## 9. 与 KBEngine 的对比

| 能力 | KBEngine | theseed |
|------|---------|---------|
| Tracing | 无 | OTel，EntityCall 自动传播 |
| Metrics | Watcher（有限） | OTel Metrics |
| Logging | LOG_MSG 文本 | OTel 结构化 JSON |
| 分布式追踪 | 无 | Jaeger/Tempo 全链路 |
| Dashboard | 无 | Grafana 预置 |
| 告警 | 无 | Alertmanager |
| Debug | Telnet pyExec | DAP 协议，IDE 原生 |
| Profiling | 手动 | 零成本 probe + 火焰图 |
| 运维控制面 | Watcher 混合承载 | 拆到 `02-ops-control-plane` |
