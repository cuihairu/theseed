# MachineAgent & Host Ops - Host Reporting, Process Orchestration, and Node Summary

> English | **中文**

> Origin: BigWorld's `bwmachined`.
> Reference implementation: the component scheduling and status aggregation path of KBEngine's `machine`.
> This layer is "host agent + local process orchestration + node summary reporting"; it is not a complete monitoring center.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
BigWorld's bwmachined is more like a node daemon:
  - maintains the machine and process table
  - performs platform checks, version checks, core dump checks
  - responsible for start / stop / notification / discovery
  - also carries a certain amount of cluster-side machine announcement and state maintenance
```

### How KBEngine Implements It

```
KBEngine's machine is more like a component scheduling and state aggregation point:
  - component registration and query
  - componentID allocation
  - process start / stop / kill
  - CPU / memory sampling and reporting
```

### Pros and Cons

```
BigWorld's advantages:
  - complete node daemon responsibilities
  - more systematic coverage of machine and process state

BigWorld's drawbacks:
  - the machine agent can easily be made too heavy
  - requires stricter separation of the control plane and the observation plane

KBEngine's advantages:
  - machine semantics are more direct
  - closer to the component orchestration entry point

KBEngine's drawbacks:
  - the boundary between monitoring and control is still coarse
  - resource sampling is more of a point capability
```

### theseed's Trade-offs

```
theseed keeps a single MachineAgent,
but does not make it a full-featured monitoring center.

It is only responsible for:
  - host resource summary
  - local process orchestration
  - health state reporting back
  - controlled node-level command execution
```

---

## 0. Design Boundary

This document covers:

```
  - host summary sampling such as CPU / memory / disk / network
  - local process table, ports, version, binary existence information
  - local actions such as spawn / stop / restart / drain / retire
  - reporting the node summary to the Ops Control Plane
```

This document does not cover:

```
  - long-term metric storage
  - metric aggregation and alerting rules
  - trace / flamegraph / profile storage and analysis
  - a cross-cluster global monitoring center
```

Its relationship with the other documents:

```
05-telemetry-and-debug
  - responsible for detailed metrics, logs, traces, flame graphs

04-ops-control-plane
  - responsible for node summary query, controlled commands, audit

03-cluster-lifecycle
  - responsible for advancing drain / retire / controlled shutdown state
```

---

## 1. Why machine Cannot Become a Monitoring Center

If machine were built as a monitoring center, it would simultaneously swallow three categories of responsibility:

```
1. observation collection
2. ops control
3. state aggregation
```

This would distort the boundaries:

```
  - sampling frequency and control commands pollute each other
  - historical storage couples with the local agent
  - machine-level responsibilities start to cover cluster-level responsibilities
```

A more reasonable split is:

```
MachineAgent
  - samples and reports the current node state
  - executes local-level actions

Ops Control Plane
  - aggregates multiple node summaries
  - provides ops query and command entry points

Telemetry
  - keeps high-frequency, fine-grained, traceable diagnostic data
```

So machine may look like a simple monitoring center,
but essentially it should only provide:

```
NodeSummary / HostSummary / ProcessSummary
```

rather than a real monitoring backend.

---

## 2. MachineAgent Responsibilities

### 2.1 Host Summary

It is recommended to include at minimum:

```
  - host id / hostname / platform
  - cpu usage rate
  - memory total / used / available
  - disk capacity / remaining / pressure marker
  - network basic throughput summary
  - load average or equivalent metric
```

### 2.2 Process Summary

```
  - pid / component type / component id
  - binary path / version / start time
  - port occupancy
  - running state / health state
  - core dump / crash markers
```

### 2.3 Local Orchestration

```
  - start process
  - stop process
  - restart process
  - trigger drain
  - assist retire / controlled shutdown
```

### 2.4 Node Summary Reporting

```
  - periodically report MachineSnapshot
  - event-based reporting of process birth / death / crash
  - leave high-frequency details to Telemetry and low-frequency summary to Ops
```

---

## 3. Recommended Data Model

```cpp
struct HostSummary {
    std::string hostname;
    double cpuUsage = 0.0;
    double memoryUsage = 0.0;
    double diskUsage = 0.0;
    double loadAverage = 0.0;
    uint64_t networkRxBytes = 0;
    uint64_t networkTxBytes = 0;
};

struct ProcessSummary {
    std::string name;
    uint32_t pid = 0;
    uint32_t port = 0;
    std::string version;
    bool healthy = false;
};

struct NodeSummary {
    HostSummary host;
    std::vector<ProcessSummary> processes;
    bool draining = false;
    bool overloaded = false;
};
```

The principles for these structs are:

```
  - summary-oriented, not complete-diagnosis-oriented
  - current-state-oriented, not historical-time-series-oriented
  - control-plane-consumption-oriented, not direct-persistence-oriented
```

---

## 4. Interface Boundary

### 4.1 Sampling Interface

```cpp
class IHostProbe {
public:
    virtual ~IHostProbe() = default;
    virtual HostSummary sample() = 0;
};
```

### 4.2 Process Management Interface

```cpp
class IProcessSupervisor {
public:
    virtual ~IProcessSupervisor() = default;
    virtual bool start(const std::string& target) = 0;
    virtual bool stop(uint32_t pid) = 0;
    virtual bool restart(uint32_t pid) = 0;
};
```

### 4.3 Node Agent Interface

```cpp
class IMachineAgent {
public:
    virtual ~IMachineAgent() = default;
    virtual NodeSummary snapshot() = 0;
    virtual void report() = 0;
    virtual bool execute(const std::string& command, const JsonValue& args) = 0;
};
```

---

## 5. Comparison with BigWorld / KBEngine

### BigWorld

```
bwmachined's characteristics:
  - more like a machine-level daemon and cluster entry point
  - responsible for discovery as well as start and notification
  - also handles some platform checks and state maintenance
```

### KBEngine

```
machine's characteristics:
  - more like a component scheduling and state aggregation entry point
  - CPU / memory sampling is already within its responsibilities
  - directly bound to component registration and query
```

### theseed

```
theseed converges into a single MachineAgent:
  - only host-level summary and local orchestration
  - the global ops view goes to the Ops Control Plane
  - detailed diagnosis goes to Telemetry
```

---

## 6. The Correct Way to Build a Simple Monitoring Center

If a role like a simple monitoring center is truly needed,
it is recommended to build the following chain:

```
MachineAgent
  -> reports NodeSummary
  -> Ops Control Plane aggregates
  -> /status/summary presents externally
```

This yields:

```
  - node health overview
  - resource pressure summary
  - process online state
  - drain / retire / overload markers
```

but machine will not become:

```
  - a Grafana replacement
  - a Metrics storage backend
  - a Trace storage backend
  - an alerting orchestration center
```

---

## 7. Phased Boundaries

### MVP

```
  - host cpu / memory / disk summary
  - process list / state / pid
  - local start / stop / restart
  - node-level health summary
```

### Phase 2

```
  - core dump / crash markers
  - port conflict / binary version / platform probe
  - more complete drain / retire coordination
```

### Phase 3

```
  - richer cross-node discovery
  - finer-grained machine labels and policies
  - tighter integration with automated ops policies
```

---

## 8. One-Sentence Judgment

```
machine should be the node agent and the local orchestration entry point,
not a full-featured monitoring center.
```
