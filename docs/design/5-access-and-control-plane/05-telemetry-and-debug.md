# Telemetry & Debug — 遥测、调试与诊断边界

> 来源源头：BigWorld `Watcher / message_logger / profiler` 的诊断思想。
> 参考实现：KBEngine 的基础日志、观察与调试路径。
> theseed 采用 OTel / OTLP 作为现代化遥测方案。
> 本篇只讨论 Telemetry、Debug 和 Diagnostics，不替代运维控制面。

---

## 0.5 引擎实现对照与取舍

### BigWorld 是怎么实现的

```
BigWorld 的观测与诊断能力主要围绕：
  - Watcher
  - message logger
  - profiler
  - entity profiler
```

### KBEngine 是怎么实现的

```
KBEngine 也有基础的日志、调试和观察能力，
但更偏轻量集成，没有 BigWorld 那么完整的系统化诊断链。
```

### 优缺点

```
BigWorld 的优点：
  - 诊断链成熟
  - 对系统状态观察很强

KBEngine 的优点：
  - 简单
  - 不会把观测体系做重

共同缺点：
  - 观测一旦和控制面混在一起，会影响边界清晰度
```

### theseed 的取舍

```
theseed 把 Telemetry 和 Ops Control Plane 分离，
火焰图、trace、metrics 归 Telemetry，
在线命令和状态查询归 Ops。
```

---

## 0. 设计边界

本篇负责：

```
  - traces / metrics / logs
  - debug hooks
  - 面向诊断的 profiling
```

本篇不负责：

```
  - 脚本断点 / 单步 / 栈调试
  - 在线命令执行
  - 分布式状态树查询
  - challenge / ban / shutdown 控制入口
  - profiler 如何反向驱动 load balance
```

后两类能力见：

```
04-ops-control-plane
../3-cluster-and-availability/04-runtime-profiler-and-load-feedback
../7-scripting-and-client/03-script-debug
```

---

## 1. 为什么单独叫 Telemetry

旧目录里的 `observability` 最大的问题，是容易让人误以为：

```
OTel = BigWorld Watcher 的全部替代
```

这并不准确。

更合理的拆法是：

```
Telemetry / Debug
  回答“发生了什么、怎么定位”

Ops Control Plane
  回答“在线怎么查、怎么改、怎么执行命令”

Runtime Load Feedback
  回答“系统如何据此自动调度”
```

---

## 2. Telemetry 架构

```text
Instrumentation
  ├─ C++ runtime probes
  ├─ script probes
  └─ system probes
      ▼
OTel SDK / OTLP Exporter
      ▼
OTel Collector
      ▼
Jaeger / Tempo + Prometheus + Loki
      ▼
Grafana / Alerting
```

---

## 3. Tracing

### 3.1 关键链路

```
process.tick
entity.create / destroy / migrate
entitycall.send / recv / execute
script.execute / timer / reload
aoi.update
db.load / save / query
```

### 3.2 EntityCall Trace Context

```cpp
Tracing::injectContext(msg.mutableBundle());

auto parentCtx = Tracing::extractContext(msg.bundle());
auto span = Tracing::startChildSpan("entitycall.recv", ...);
```

意义在于：

```
跨进程 EntityCall 可以作为同一条 trace 继续传播，
这和传统游戏服务端的孤立日志非常不同。
```

---

## 4. Metrics 与 Logs

### 4.1 最小指标集

```
tick_duration_ms
entity_count
queue_backlog
transport_backpressure
db_load_ms / db_save_ms
script_error_count
login_pending_count
challenge_failure_count
```

### 4.2 日志要求

```
  - 结构化
  - 可关联 trace / span id
  - 可按 process / entity / realm / request 过滤
```

---

## 5. Debug Hooks

```cpp
class IDebugProvider {
public:
    virtual void onEntityCreated(EntityId id, const std::string& entityType) = 0;
    virtual void onEntityDestroyed(EntityId id) = 0;
    virtual void onMessageSent(EntityId from, EntityId to, const std::string& method) = 0;
    virtual std::string inspectEntity(EntityId id) = 0;
};
```

设计重点：

```
Debug hook 是诊断面能力，
不是在线管理命令入口。
```

---

## 6. Diagnostics Profiling

本篇里的 profiling 只讨论：

```
  - 慢 tick 诊断
  - flamegraph 采样
  - 分阶段开销归因
```

这些诊断产物属于 Telemetry / Diagnostics，
但谁可以触发采样、谁可以下载结果，归 `04-ops-control-plane`。

它不等于：

```
BigWorld 的 EntityProfiler → loadBalance / overload gate 反馈链
```

后者属于：

`../3-cluster-and-availability/04-runtime-profiler-and-load-feedback`

---

## 7. 分阶段边界

```text
MVP：
  - 结构化 logs
  - 基础 metrics
  - 关键 traces

Phase 2：
  - 更完整的 debug bridge
  - 更强的采样策略
  - 更成熟的 profiling 导出

Phase 3：
  - 与统一控制面、自动化运维、容量平台联动
```

---

## 8. 与 BigWorld / KBEngine / theseed 的对比

| 维度 | BigWorld / KBEngine | theseed |
|------|------|------|
| 分布式 tracing | 基本无统一方案 | OTel trace |
| metrics | Watcher / 自定义统计 | OTel metrics |
| logs | 文本与进程聚合 | 结构化 logs |
| debug | 较分散 | Debug hooks + 观察器思路 |
| profiler feedback | 有系统内链路 | 拆到 cluster-and-availability |

---

## 9. 一句话判断

本篇强调的是：

```
Telemetry 只是服务端系统面的一个子层，
它既不等于 Watcher 控制面，也不等于 BigWorld 的负载反馈闭环。
```
