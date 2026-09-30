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

### 2.1 控制面 OTLP trace 导出（已落地）

`control::machine::OtlpTraceExporter` 把既有 `SpanScope` / `SpanEmitter`
产物以 OTLP/HTTP JSON（`application/json`，protobuf 编码不用）导出到
可配置 endpoint。metrics 导出见 §2.2（同族落地，拉模型）。

**数据外发默认关闭（硬性要求）**：`Config.enabled` 缺省 `false`，不显式
置 true 就不安装、不外发、零网络副作用。开启且 endpoint 合法后，启动
日志 `otlp.traces.export.enabled` 携带 `data_scope` 与批量面属性
（`batch_size` / `flush_interval_ms` / `max_attempts`）明示导出范围与
外发口径，与本节同源：

| 导出 | 不导出 |
| --- | --- |
| 已完结 span 的 name | 结构化日志 |
| traceId / spanId / parentSpanId | metrics |
| startTimeUnixNano / endTimeUnixNano | 请求与审计载荷 |
| attributes（key + 分型值：string / int64 / double / bool） | |
| resource `service.name`（缺省 `theseed`） | |
| scope `theseed.control.tracing`（kind=INTERNAL，status 不写=UNSET） | |

**endpoint 口径**：仅 `http://<IPv4 字面量>[:port][/path]`，端口缺省
4318（OTLP/HTTP 约定）、path 缺省 `/`。仓内 transport（`TcpConnection`）
只做 IPv4 直连，无 DNS 与 TLS——需域名或加密时在 endpoint 前置本地
代理（如Collector 收 4318 明文再转发）。IPv4 解析与 `TcpConnection`
的 `inet_pton` 同口径（拒绝前导零与越界段）。

**传输与编码**：零第三方依赖（不引 OTel SDK / protobuf / abseil——
vcpkg 工具链未装，引入需全量重建，沿既定降级路径）。**异步批量外发**：
发射钩子只把 span 入队（链式转发仍在发射线程同步完成），后台 worker
线程按 `Config.batchSize`（缺省 32）满批或 `Config.flushInterval`
（缺省 1000ms）兜底周期取批，一次 POST 编码整批（`resourceSpans` 单
资源 + `spans` 数组）；空队列不发包、不忙等。`Config.timeout`（缺省
500ms）即单次 POST 的阻塞上界，发射钩子不再随网络等待。

**失败语义**：建连失败 / 对端先关 / 超时 / 非 2xx 一律丢弃该批并
`++exportFailed`（按批内 span 数计），warn 日志
`otlp.traces.export.failed` 带 `http_status`（0 = 未收到响应）；
**有界重试**：每批共 `Config.maxAttempts`（缺省 3）次尝试、相邻间隔
`Config.retryBackoff`（缺省 50ms），不按状态码分流；停机排水只试一次
（退出优先）。2xx 计 `exportedOk`（按批内 span 数计），成功路径零
日志。队列满（`Config.maxQueue`，缺省 4096）丢新 span 并计
`exportFailed`——本地过载只计数不刷屏。安装时链式保留宿主既有
`SpanEmitter`（导出后照常转发），`uninstall()` 与析构还原槽位并
join worker（排空残余队列后退出，卸载即同步收口点）。

**配置面**：`MachineDaemon::Config::otlpTrace` 承载（daemon 是控制面
span 属主；当前无生产宿主装配 daemon，配置面先行，装配随宿主落地）。
开启即全量导出，无采样。

### 2.2 控制面 OTLP metrics 导出（已落地）

`control::machine::OtlpMetricsExporter` 把进程级 `MetricsRegistry`
（本文 §4 的指标面，`machine_*` / `ops_*` / 基础设施计数与直方图）全量
快照以 OTLP/HTTP JSON 导出到可配置 endpoint。与 §2.1 同族同约束（零第
三三方依赖、endpoint 口径、失败语义、注入传输接缝均沿用，公共面单实现
`OtlpTraceExporter` 复用），差异只在数据源与驱动模型：trace 是事件驱动
（span 完结钩子），metrics 是**周期拉取**——`MachineDaemon::tick()` 按
`Config.interval`（缺省 5000ms）驱动 `exportIfDue()`，到期一次 POST 上
报当期全量快照（每期一笔请求，批内含全部样本）；首个到期调用立即导出
（与 `reportIfDue` 首 tick 立即上报同口径）。

**数据外发默认关闭（硬性要求）**：`Config.enabled` 缺省 `false`，不显式
置 true 就不安装、不外发、零网络副作用（未开启时 `exportIfDue` 空转，
连注册表快照都不取）。开启且 endpoint 合法后，启动日志
`otlp.metrics.export.enabled` 携带 `interval_ms` 与 `data_scope` 属性
明示导出范围，与本节同源：

| 导出 | 不导出 |
| --- | --- |
| metric name 与 description | spans/traces |
| counter / gauge 值（asInt，int64 口径含负值） | 结构化日志 |
| histogram count / sum / bucketCounts / explicitBounds（累积口径，同仓内注册表） | 请求与审计载荷 |
| 采集时刻 timeUnixNano（快照时刻） | |
| resource `service.name`（缺省 `theseed`） | |
| scope `theseed.control.metrics` | |

**类型映射**：counter → OTLP `sum`（`aggregationTemporality=2` 累积 +
`isMonotonic=true`）；gauge → `gauge`；histogram → `histogram`（累积
`bucketCounts` 与 `explicitBounds`，`bucketCounts` 长度 = 边界数 + 1，
末桶 `+Inf`——与注册表内部语义逐一对应）。注册表不记每指标起始时刻，
`startTimeUnixNano` 缺省 0（protobuf 默认值，采集端按累计口径处理）；
非有限 histogram sum（NaN/Inf）无合法 JSON 数字表示，按 OTLP 可选字段
省略（count / buckets 不受影响）。

**endpoint / 传输 / 失败语义**：与 §2.1 同口径——仅
`http://<IPv4 字面量>[:port][/path]`（缺省 4318）；`TcpConnection`
一次性阻塞 POST；`Config.timeout`（缺省 500ms）即 tick 上下文的阻塞上
界；失败（建连拒 / 对端先关 / 超时 / 非 2xx）丢弃 + `++exportFailed` +
warn `otlp.metrics.export.failed` 带 `http_status`（0 = 未收到响应），
**当期不重试**；2xx 计 `exportedOk`，成功路径零日志。`install()` 只做
校验与启动声明，不触碰任何全局槽位；`uninstall()` 与析构兜底收口。

**配置面**：`MachineDaemon::Config::otlpMetrics` 承载（daemon tick 驱
动导出；当前无生产宿主装配 daemon，配置面先行，装配随宿主落地）。
开启即全量导出，无采样、无过滤。

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
