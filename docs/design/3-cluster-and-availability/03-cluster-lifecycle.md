# Cluster Lifecycle — Service Fragment, Retire, Drain, and Controlled Shutdown

> **English** | [中文](03-cluster-lifecycle.zh.md)

> Origin: BigWorld `ServiceApp / registerServiceFragment / retireApp / controlledShutDown`.
> KBEngine only provides a more basic component lifecycle reference.
> This layer describes the "cluster lifecycle surface", not the Runtime main path, nor a pure disaster-recovery document.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
BigWorld has a very complete cluster lifecycle model:
  - Service Fragment registration
  - retire / drain
  - controlled shutdown
  - Coordinated exit of multiple process roles
```

### How KBEngine Implements It

```
KBEngine has component lifecycle management,
but not as complete a retire / fragment system as BigWorld.
```

### Pros and Cons

```
BigWorld's advantages:
  - Ops actions are semantic
  - The shutdown and evacuation chain is more controllable

KBEngine's advantages:
  - Simple
  - Component lifecycle is more direct

Shared disadvantages:
  - The more complete the lifecycle, the more complex the implementation and validation
```

### theseed's Trade-offs

```
theseed makes Cluster Lifecycle a separate document,
because it is a BigWorld-level system-layer difference,
and can no longer be buried in scattered fault-tolerance or ops chapters.
```

### Why It Cannot Be Merged Back into Disaster Recovery or Ops

```
If Cluster Lifecycle were merged back into Fault Tolerance,
it would mix "recovery" and "online retirement / shutdown orchestration" into one thing.

If Cluster Lifecycle were merged back into Ops,
it would mix "the execution entry point" and "system state advancement" into one thing.

So it must be independent:
  - Fault Tolerance discusses what to do after a crash
  - Ops discusses who issues the commands
  - Cluster Lifecycle discusses how the system enters the next state
```

---

## 0. Design Boundary

```
Cluster Lifecycle covers:
  - Processes joining / leaving the cluster
  - drain / retire / shutdown coordination
  - service fragment registration and liveness constraints
  - Multi-role shutdown stage advancement

Cluster Lifecycle does not cover:
  - The EntityCall main path
  - AOI / witness / ghost synchronization
  - The primary persistence interface
  - metrics / traces storage
```

Its relationship to other documents:

```
01-fault-tolerance
  - Leans more toward recovery and disaster tolerance

This document
  - Leans more toward online lifecycle coordination and shutdown orchestration
```

---

## 1. Why This Layer Must Be Independent

BigWorld's system layer has a very obvious capability line:

```
1. BaseApp / ServiceApp can be retired
2. Service fragment can be registered / deregistered
3. Service absence can trigger a shutdown policy
4. controlled shutdown has stage advancement
5. LoginApp / DBApp / BaseApp / CellAppMgr all participate
```

If this layer is not established separately, the document will have two problems:

```
1. Treating drain / retire as a deployment script detail
2. Treating shutdown as a "disaster-recovery supplementary note"
```

In reality it is an independent cluster control plane.

More directly:

```
drain / retire / controlled shutdown
are not deployment script tricks,
but an engine-level state machine.
```

---

## 2. Core Concepts

### 2.1 Drain

```
Meaning:
  - The process stops accepting new traffic
  - Allows in-flight work to complete
  - Waits for migration, archiving, session transfer
```

### 2.2 Retire

```
Meaning:
  - The process is preparing to leave the cluster
  - Must drain first
  - Must handle residual entity / session / service fragment
```

### 2.3 Controlled Shutdown

```
Meaning:
  - Cluster-coordinated phased shutdown
  - Each role executes in order
  - Not a simple kill process
```

### 2.4 Service Fragment

```
Meaning:
  - An instance of some logical service capability
  - Can be deployed on a dedicated ServiceApp or a special BaseApp
  - Its lifecycle is independent of ordinary player entities
```

---

## 3. Service Fragment Model

### 3.1 Responsibilities

Service Fragment is used to carry:

```
  - Global service logic
  - Matchmaking / ranking / mail / event scheduling
  - Long-lived capabilities not suited to being bound to an ordinary player Base
```

### 3.2 Registration Relationship

```
Service Fragment Instance
  → belongs to Process
  → advertises ServiceName
  → registers to Cluster Lifecycle Registry
```

### 3.3 Design Requirements

```
1. Service registration must be explicit
2. A service may have multiple fragments
3. Whether the disappearance of a fragment triggers shutdown must be a policy configuration
4. Must not mistake "this entity exists" for "the service is available"
```

---

## 4. Core Interfaces

### 4.1 Lifecycle Interface

```cpp
// cluster/IClusterLifecycleManager.h

enum class LifecycleState {
    Joining,
    Running,
    Draining,
    Retiring,
    ShuttingDown,
    Stopped
};

class IClusterLifecycleManager {
public:
    virtual ~IClusterLifecycleManager() = default;

    virtual Future<void> setDraining(ProcessId id, bool value) = 0;
    virtual Future<void> retire(ProcessId id) = 0;
    virtual Future<void> controlledShutdown(ShutdownStage stage) = 0;
    virtual LifecycleState state(ProcessId id) const = 0;
};
```

### 4.2 Service Registry

```cpp
// cluster/IServiceFragmentRegistry.h

struct ServiceFragmentInfo {
    std::string serviceName;
    ProcessId processId;
    FragmentId fragmentId;
    bool healthy = true;
};

class IServiceFragmentRegistry {
public:
    virtual ~IServiceFragmentRegistry() = default;

    virtual Future<void> registerFragment(
        const ServiceFragmentInfo& info) = 0;
    virtual Future<void> deregisterFragment(
        const std::string& serviceName,
        FragmentId fragmentId) = 0;

    virtual std::vector<ServiceFragmentInfo> list(
        const std::string& serviceName) const = 0;
};
```

---

## 5. Process State Machine

```
Joining
  → Running
  → Draining
  → Retiring
  → Stopped

Exception branches:
Running → ShuttingDown
Draining → ShuttingDown
Retiring → ShuttingDown
```

Constraints:

```
1. Only Running is allowed to take on new traffic
2. After Draining, no new sessions / new entity assignments may be accepted
3. Before Retiring, critical state must be completed or transferred
4. Service Fragment deregistration must complete before the process finally exits
```

---

## 6. Controlled Shutdown Stages

theseed is recommended to explicitly adopt phased shutdown, rather than a boolean switch.

```
Stage 1: Request
  - Broadcast the shutdown intent
  - Reject new logins

Stage 2: Drain
  - Gateway / Login / Session enter draining
  - Base / Service begin migration and cleanup

Stage 3: Persist
  - Archiver / LocalArchiveStore flush
  - Critical sessions and entity state are flushed to disk

Stage 4: Perform
  - Stop the process main loop
  - Deregister fragment / routing / registration info

Stage 5: Complete
  - The whole cluster confirms completion
```

Requirements:

```
1. Every stage must be observable
2. Failure at any stage must give an operator-visible reason
3. It is not allowed to skip Persist and exit crudely
```

---

## 7. Relationship with Hot Update / Release

A rolling update is essentially the repeated process of "controlled retirement + new instance joining".

Therefore:

```
rolling update
  = join new
  + drain old
  + retire old
```

The `drain → migrate → restart` in the hot update document must not be treated as a local trick; it should reuse the lifecycle model of this document.

---

## 8. Failure Policy

### 8.1 Service Absence

For a Service Fragment, policy-based handling must be supported:

```yaml
cluster_lifecycle:
  service_failure_policy:
    mail_service: degrade
    match_service: degrade
    payment_callback: shutdown
```

Three policy categories:

```
ignore
  - Alert only, do not shut down

degrade
  - Mark the service unavailable, degrade the business

shutdown
  - Trigger controlled shutdown
```

### 8.2 Retirement Failure

```
If drain times out:
  - Allow the operator to keep waiting
  - Allow forcing entry into shutdown
  - Must output a summary of the blocking reason
```

---

## 9. Relationship with Ops Control Plane

This document defines the lifecycle model; the command entry should go through the `Ops Control Plane`.

Correspondence:

| Lifecycle action | Command entry |
|------|------|
| set draining | Ops Control Plane |
| retire process | Ops Control Plane |
| controlled shutdown | Ops Control Plane |
| inspect fragment registry | Ops Control Plane |

In other words:

```
Lifecycle defines semantics
Ops Control Plane provides the entry point and audit
```

---

## 10. Phased Boundaries

```
MVP:
  - set draining
  - retire process
  - Basic controlled shutdown
  - Does not introduce a complete ServiceApp system

Phase 2:
  - Service Fragment Registry
  - fragment failure policy
  - Interlock with the release system

Phase 3:
  - Finer-grained shutdown stage
  - Cross-realm lifecycle orchestration
```

---

## 11. Comparison with BigWorld / KBEngine

| Dimension | BigWorld | KBEngine | theseed |
|------|---------|---------|---------|
| retireApp | Yes | Weaker | Explicitly modeled |
| controlled shutdown | Complete stages | Weaker | Explicitly phased |
| Service Fragment | Yes | Basically none | Phase 2 goal |
| service death policy | Yes | Weaker | Explicit policy configuration |
| rolling update semantics | Supported by the system | Weaker | Reuses the lifecycle model |
