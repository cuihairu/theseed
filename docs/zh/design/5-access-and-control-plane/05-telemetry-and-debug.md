# Telemetry & Debug — Telemetry, Debug, and Diagnostics Boundary

> English | **中文**

> Origin: the diagnostic thinking of BigWorld's `Watcher / message_logger / profiler`.
> Reference implementation: KBEngine's basic logging, observation, and debug path.
> theseed adopts OTel / OTLP as the modern telemetry solution.
> This document only discusses Telemetry, Debug, and Diagnostics; it does not replace the ops control plane.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
BigWorld's observation and diagnosis capabilities mainly revolve around:
  - Watcher
  - message logger
  - profiler
  - entity profiler
```

### How KBEngine Implements It

```
KBEngine also has basic logging, debugging, and observation capabilities,
but leans more toward lightweight integration, without BigWorld's complete systematic diagnosis chain.
```

### Pros and Cons

```
BigWorld's advantages:
  - mature diagnosis chain
  - very strong observation of system state

KBEngine's advantages:
  - simple
  - does not make the observation system heavy

Shared drawbacks:
  - once observation is mixed with the control plane, it affects boundary clarity
```

### theseed's Trade-offs

```
theseed separates Telemetry and the Ops Control Plane:
flame graphs, traces, and metrics belong to Telemetry,
online commands and state query belong to Ops.
```

---

## 0. Design Boundary

This document covers:

```
  - traces / metrics / logs
  - debug hooks
  - diagnosis-oriented profiling
```

This document does not cover:

```
  - script breakpoints / single stepping / stack debugging
  - online command execution
  - distributed state tree query
  - challenge / ban / shutdown control entry points
  - how the profiler reversely drives load balance
```

The latter two categories of capability are covered in:

```
04-ops-control-plane
../3-cluster-and-availability/04-runtime-profiler-and-load-feedback
../7-scripting-and-client/03-script-debug
```

---

## 1. Why It Is Called Telemetry Separately

The biggest problem with `observability` in the old directory is that it easily leads people to assume:

```
OTel = a complete replacement for BigWorld Watcher
```

This is not accurate.

A more reasonable split is:

```
Telemetry / Debug
  answers "what happened and how to locate it"

Ops Control Plane
  answers "how to query online, how to change, how to execute commands"

Runtime Load Feedback
  answers "how the system schedules automatically based on this"
```

---

## 2. Telemetry Architecture

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

### 2.1 Control Plane OTLP Trace Export (Already Landed)

`control::machine::OtlpTraceExporter` exports the existing `SpanScope` / `SpanEmitter`
artifacts as OTLP/HTTP JSON (`application/json`; protobuf encoding is not used) to a
configurable endpoint. For metrics export see §2.2 (same family, landed, pull model).

**Data export is off by default (hard requirement)**: `Config.enabled` defaults to `false`; unless it is
explicitly set to true, nothing is installed, nothing is exported, and there are zero network side effects.
Once enabled and the endpoint is valid, the startup log `otlp.traces.export.enabled` carries `data_scope`
and the batch-plane attributes (`batch_size` / `flush_interval_ms` / `max_attempts`) to explicitly state
the export scope and the export definition, same origin as this section:

| Exported | Not Exported |
| --- | --- |
| name of completed spans | structured logs |
| traceId / spanId / parentSpanId | metrics |
| startTimeUnixNano / endTimeUnixNano | request and audit payloads |
| attributes (key + typed value: string / int64 / double / bool) | |
| resource `service.name` (default `theseed`) | |
| scope `theseed.control.tracing` (kind=INTERNAL, status omitted=UNSET) | |

**Endpoint definition**: only `http://<IPv4 literal>[:port][/path]`, port defaults to 4318 (OTLP/HTTP
convention), path defaults to `/`. The in-repo transport (`TcpConnection`) only does direct IPv4 connections,
with no DNS and no TLS — when a hostname or encryption is needed, put a local proxy in front of the endpoint
(for example, the Collector receives plaintext on 4318 and forwards it). IPv4 resolution follows the same
definition as `TcpConnection`'s `inet_pton` (leading zeros and out-of-range octets are rejected).

**Transport and encoding**: zero third-party dependencies (no OTel SDK / protobuf / abseil — the vcpkg toolchain
is not installed, and introducing them requires a full rebuild, so the established fallback path is followed).
**Async batched export**: the emission hook only enqueues spans (chained forwarding still completes synchronously
on the emission thread); the background worker thread takes a batch when `Config.batchSize` (default 32) is full
or on the `Config.flushInterval` (default 1000ms) fallback cycle, and encodes the whole batch in one POST (`resourceSpans`
single resource + `spans` array); an empty queue sends nothing and does not busy-wait. `Config.timeout` (default 500ms)
is the blocking upper bound for a single POST, and the emission hook no longer waits on the network.

**Failure semantics**: connection failure / peer closes first / timeout / non-2xx — all of them discard the batch and
`++exportFailed` (counted by the number of spans in the batch), with a warn log `otlp.traces.export.failed`
carrying `http_status` (0 = no response received); **bounded retry**: each batch gets `Config.maxAttempts`
(default 3) attempts in total with `Config.retryBackoff` (default 50ms) between adjacent attempts, not split
by status code; shutdown draining tries only once (exit takes priority). 2xx counts as `exportedOk`
(counted by the number of spans in the batch), and the success path emits zero logs. When the queue is
full (`Config.maxQueue`, default 4096), new spans are dropped and counted as `exportFailed` — local overload
only increments counters without flooding the log. On install, the host's existing `SpanEmitter` is
preserved in the chain (forwarding proceeds as usual after export); `uninstall()` and the destructor restore
the slot and join the worker (which exits after draining the residual queue, making unload a synchronous closing point).

**Configuration plane**: carried by `MachineDaemon::Config::otlpTrace` (the daemon is the owner of
control plane spans; currently no production host assembles the daemon, so the configuration plane comes
first and assembly lands with the host). Once enabled, everything is exported in full, with no sampling.

### 2.2 Control Plane OTLP Metrics Export (Already Landed)

`control::machine::OtlpMetricsExporter` exports the process-level `MetricsRegistry` (the metric plane of this
document's §4, `machine_*` / `ops_*` / infrastructure counters and histograms) as a full snapshot in OTLP/HTTP
JSON to a configurable endpoint. It belongs to the same family and follows the same constraints as §2.1 (zero
third-party dependencies, endpoint definition, failure semantics, and the injected transport seam are all reused;
the common plane is implemented once and `OtlpTraceExporter` is reused), with differences only in the data source
and the driving model: traces are event-driven (span completion hooks), while metrics are **periodically pulled**
— `MachineDaemon::tick()` drives `exportIfDue()` according to `Config.interval` (default 5000ms), and when due, a
single POST reports the current full snapshot (one request per period, with all samples in the batch); the first
due call exports immediately (same definition as `reportIfDue` reporting immediately on the first tick).

**Data export is off by default (hard requirement)**: `Config.enabled` defaults to `false`; unless it is
explicitly set to true, nothing is installed, nothing is exported, and there are zero network side effects
(when not enabled, `exportIfDue` spins idle and does not even take a registry snapshot). Once enabled and
the endpoint is valid, the startup log `otlp.metrics.export.enabled` carries the `interval_ms` and
`data_scope` attributes to explicitly state the export scope, same origin as this section:

| Exported | Not Exported |
| --- | --- |
| metric name and description | spans/traces |
| counter / gauge values (asInt, int64 definition including negatives) | structured logs |
| histogram count / sum / bucketCounts / explicitBounds (cumulative definition, same as the in-repo registry) | request and audit payloads |
| collection time timeUnixNano (snapshot time) | |
| resource `service.name` (default `theseed`) | |
| scope `theseed.control.metrics` | |

**Type mapping**: counter → OTLP `sum` (`aggregationTemporality=2` cumulative +
`isMonotonic=true`); gauge → `gauge`; histogram → `histogram` (cumulative `bucketCounts` and
`explicitBounds`, `bucketCounts` length = number of bounds + 1, last bucket `+Inf` — corresponding
one by one with the registry's internal semantics). The registry does not record a start time per
metric, so `startTimeUnixNano` defaults to 0 (the protobuf default; the collection side handles it
as a cumulative definition); a non-finite histogram sum (NaN/Inf) has no valid JSON number
representation and is omitted as an OTLP optional field (count / buckets are unaffected).

**endpoint / transport / failure semantics**: same definition as §2.1 — only `http://<IPv4 literal>[:port][/path]`
(default 4318); `TcpConnection` does a one-shot blocking POST; `Config.timeout` (default 500ms) is the blocking
upper bound for the tick context; on failure (connection refused / peer closes first / timeout / non-2xx)
the batch is discarded + `++exportFailed` + warn `otlp.metrics.export.failed` carrying `http_status`
(0 = no response received), **no retry in the current period**; 2xx counts as `exportedOk`, and the
success path emits zero logs. `install()` only validates and declares startup, and does not touch any
global slot; `uninstall()` and the destructor provide fallback closing.

**Configuration plane**: carried by `MachineDaemon::Config::otlpMetrics` (the daemon tick drives the export;
currently no production host assembles the daemon, so the configuration plane comes first and assembly lands
with the host). Once enabled, everything is exported in full, with no sampling and no filtering.

---

## 3. Tracing

### 3.1 Key Links

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

The significance lies in:

```
Cross-process EntityCall can continue to propagate as the same trace,
which is very different from the isolated logs of traditional game servers.
```

---

## 4. Metrics and Logs

### 4.1 Minimal Metric Set

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

### 4.2 Log Requirements

```
  - structured
  - correlatable with trace / span id
  - filterable by process / entity / realm / request
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

Design focus:

```
The debug hook is a diagnostic-plane capability,
not an online management command entry point.
```

---

## 6. Diagnostics Profiling

The profiling in this document only discusses:

```
  - slow tick diagnosis
  - flamegraph sampling
  - phase-by-phase cost attribution
```

These diagnostic artifacts belong to Telemetry / Diagnostics,
but who may trigger sampling and who may download results belongs to `04-ops-control-plane`.

It is not equivalent to:

```
BigWorld's EntityProfiler → loadBalance / overload gate feedback chain
```

The latter belongs to:

`../3-cluster-and-availability/04-runtime-profiler-and-load-feedback`

---

## 7. Phased Boundaries

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

## 8. Comparison with BigWorld / KBEngine / theseed

| Dimension | BigWorld / KBEngine | theseed |
|------|------|------|
| Distributed tracing | basically no unified solution | OTel trace |
| metrics | Watcher / custom statistics | OTel metrics |
| logs | text and process aggregation | structured logs |
| debug | rather scattered | Debug hooks + observer thinking |
| profiler feedback | has an in-system chain | split out to cluster-and-availability |

---

## 9. One-Sentence Judgment

What this document emphasizes is:

```
Telemetry is only a sub-layer of the server-side system plane:
it is neither the Watcher control plane nor BigWorld's load feedback loop.
```
