# Runtime Profiler & Load Feedback — Load Profile, Overload Determination, and Scheduling Feedback Chain

> **English** | [中文](04-runtime-profiler-and-load-feedback.zh.md)

> Origin: BigWorld `EntityProfiler / EntityTypeProfiler / artificialMinLoad / CellAppMgr::loadBalance / overloadCheck / shouldOffload`.
> KBEngine only has a basic load judgment reference, lacking a complete profiler → scheduling feedback feedback loop.
> This document discusses "how load signals return to scheduling and throttling", not general Telemetry, nor an Ops query entry point.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
BigWorld's profiler is not just an observation tool:
  - EntityProfiler / EntityTypeProfiler produce runtime load signals
  - These signals feed back into loadBalance / shouldOffload / login throttling
```

### How KBEngine Implements It

```
KBEngine also does load judgment,
but lacks BigWorld's complete profiler → scheduling feedback chain.
```

### Pros and Cons

```
BigWorld's advantages:
  - Load signals genuinely participate in scheduling
  - The system feedback loop is more complete

KBEngine's advantages:
  - Simple
  - No need to first build a complete profiling system

Shared disadvantages:
  - Once this layer is mixed with Telemetry or Ops, the boundary becomes very blurry
```

### theseed's Trade-offs

```
theseed makes profiler feedback a separate document,
because it is neither "emitting one more set of metrics",
nor "one more online query entry point",
but the system scheduling input layer.
```

### Why It Cannot Be Merged Back into Telemetry or Ops

```
If merged back into Telemetry,
it would be mistakenly thought that the profiler's job is only "sample and display".

If merged back into Ops,
it would be mistakenly thought that the profiler's job is only "show state to humans".

But the essence of this BigWorld layer is:
  profiler output must continue into the scheduler,
  ultimately affecting offload, rebalance, login throttling.
```

---

## 0. Design Boundary

```
This document covers:
  - runtime profiler sampling objects and levels
  - Load smoothing, artificial minimum load, overload determination
  - How profiler signals enter load balancing, offload, login throttling

This document does not cover:
  - traces / metrics / logs backends
  - Online commands and watcher query entry points
  - The implementation details of the concrete Cell partitioning algorithm
```

Relationship with other documents:

```
05-telemetry-and-debug
  - Discusses Telemetry / Debug / basic Profiling

04-ops-control-plane
  - Discusses how to inspect / command

This document
  - Discusses how profiler signals become system scheduling input
```

---

## 1. Why This Layer Cannot Continue to Live in OTel

BigWorld's `EntityProfiler` is not "one more set of metrics".

It is closer to:

```
Runtime load pricer
```

Because its output affects:

```
1. entity / entity type load statistics
2. Load judgment of Cell / Space
3. loadBalance / updateRanges
4. shouldOffload
5. overloadCheck triggering login limits
```

If this layer is still written as:

```
profiling = flame graph + slow tick alerts
```

then BigWorld's system difference will continue to be underestimated.

A more accurate boundary should be:

```
Flame graphs, traces, slow tick alerts
  belong to diagnostic output

entity load, space load, overloadCheck input
  belong to system scheduling input
```

---

## 2. BigWorld's Signal Chain

BigWorld can be abstracted into the following feedback chain:

```
Entity / Base call time
  → EntityProfiler
  → EntityTypeProfiler
  → Cell / Space smoothed load
  → CellAppMgr balancing / overloadCheck
  → offload / retire / login throttling
```

The most important thing here is:

```
The Profiler does not only expose outward,
it also feeds inward to the scheduler.
```

---

## 3. Three-Level profiler Semantics

### 3.1 EntityProfiler

Responsibilities:

```
Record the execution time spent by a single entity within one tick
Output raw / smoothed / adjusted load
```

### 3.2 EntityTypeProfiler

Responsibilities:

```
Aggregate the load of entities of the same type into a type-level profile
```

### 3.3 Space / Cell / App load

Responsibilities:

```
Further aggregate entity-level load into scheduling input for CellApp / Space
```

These three levels can no longer be treated merely as "metrics label aggregation".

---

## 4. Key Concepts

### 4.1 Raw Load

```
Raw execution cost of the current tick
```

### 4.2 Smoothed Load

```
Load after exponential smoothing
Avoids a single-tick jitter directly triggering scheduling
```

### 4.3 Adjusted Load

```
Load after applying artificial correction on top of smoothed load
```

### 4.4 artificialMinLoad

A key system surface of BigWorld is:

```
The artificial minimum load can override the statistical load floor
```

Its meaning is not debugging convenience, but:

```
Injecting operational / scheduling intent such as "a world or entity should be treated as heavier even if its current CPU is not high"
into the system.
```

---

## 5. What artificialMinLoad Actually Solves

BigWorld uses `artificialMinLoad` at both the entity and space levels, showing that it solves two kinds of problems:

### 5.1 Statistical Load Is Insufficient to Express Real Cost

For example:

```
1. A certain space is still loading chunks
2. A certain entity will subsequently trigger a large batch of broadcasts or script logic
3. A certain region is quiet at present, but should not continue to take on new load
```

### 5.2 The Scheduler Needs Artificial Bias

For example:

```
1. Temporarily do not want a certain space to be pressed with more Cell work
2. A certain type of entity should "look heavier" so as to be migrated away first
```

So theseed should not mistakenly model it as a "debug variable".

---

## 6. How Load Signals Enter Scheduling

### 6.1 Balancing Input

The scheduler needs at least the following kinds of input:

```
  - entity adjusted load
  - entity-type aggregated load
  - cell / space smoothed load
  - chunk / bounds / loading state
  - artificialMinLoad overrides
```

### 6.2 Scheduling Output

In BigWorld, these signals affect:

```
  - loadBalance
  - metaLoadBalance
  - updateRanges
  - shouldOffload
  - retire underloaded cell
  - login overload status
```

This shows that the profiler is connected with lifecycle / login / topology, not an isolated module.

---

## 7. overloadCheck Is Not Just an Alert

BigWorld's `overloadCheck` has another design emphasis:

```
Overload must meet a duration threshold
before it is escalated to a system-level state
```

That is:

```
instant overload
  != admitted overload
```

This boundary is important because it avoids:

```
A single spike tick
  → immediately triggering throttling / scaling / retirement
```

theseed should explicitly write in:

```
System scheduling signals must carry duration determination
```

---

## 8. theseed's Proposed Modeling

### 8.1 Runtime profiler

```cpp
// runtime/IEntityLoadProfiler.h

struct EntityLoadSnapshot {
    EntityId entityId;
    EntityTypeId entityTypeId;
    float rawLoad = 0.0f;
    float smoothedLoad = 0.0f;
    float adjustedLoad = 0.0f;
    float artificialMinLoad = 0.0f;
};

class IEntityLoadProfiler {
public:
    virtual ~IEntityLoadProfiler() = default;

    virtual void startScope(EntityId entityId) = 0;
    virtual void stopScope(EntityId entityId) = 0;
    virtual EntityLoadSnapshot tick(EntityId entityId,
                                    Duration tickDt) = 0;
};
```

### 8.2 Type-Level Aggregation

```cpp
// runtime/IEntityTypeLoadAggregator.h

struct EntityTypeLoadSnapshot {
    EntityTypeId entityTypeId;
    uint32_t entityCount = 0;
    float rawLoad = 0.0f;
    float smoothedLoad = 0.0f;
    float maxRawLoad = 0.0f;
};

class IEntityTypeLoadAggregator {
public:
    virtual ~IEntityTypeLoadAggregator() = default;

    virtual void record(const EntityLoadSnapshot& snapshot) = 0;
    virtual EntityTypeLoadSnapshot snapshot(EntityTypeId typeId) const = 0;
};
```

### 8.3 Scheduling Feedback Interface

```cpp
// runtime/ILoadFeedbackController.h

struct RuntimeLoadStatus {
    float minLoad = 0.0f;
    float avgLoad = 0.0f;
    float maxLoad = 0.0f;
    bool anyOverloaded = false;
    bool avgOverloaded = false;
};

class ILoadFeedbackController {
public:
    virtual ~ILoadFeedbackController() = default;

    virtual void onLoadSample(const RuntimeLoadStatus& status) = 0;
    virtual void setArtificialMinLoad(SpaceId spaceId, float load) = 0;
    virtual void setShouldOffload(SpaceId spaceId, bool value) = 0;
};
```

---

## 9. Three Layers That Must Be Separated

In this chain, three layers must be distinguished:

| Layer | Question | Typical Output |
|------|------|----------|
| Telemetry | Let people see what happened | metrics / trace / logs |
| Diagnostics | Let people locate who is heavy | entity / type / space profiler |
| Control Feedback | Let the system adjust accordingly | offload / balance / throttle |

If they are mixed into one layer, the following will occur:

```
1. The OTel document looks like it already covers the BigWorld profiler
2. But in fact it does not define how the profiler affects scheduling
3. Ops sees the metrics, but the system itself has no feedback loop
```

---

## 10. Relationship with Login / Lifecycle / Topology

### 10.1 With Login Service

```
overload admitted
  → update login gate state
  → trigger throttling or pause new logins
```

### 10.2 With Cluster Lifecycle

```
underloaded group
  → retire candidate

overloaded group
  → add cell / rebalance candidate
```

### 10.3 With Topology

```
space / cell load
  → updateRanges / loadBalance
```

In other words, this document actually fills in BigWorld's "in-system feedback loop".

---

## 11. Phased Boundaries

```
MVP:
  - Only retains basic tick / process profiler
  - Does not commit to a systematic feedback loop for entity / type / space

Phase 2:
  - Introduce entity / entity-type load profiler
  - Introduce overload duration determination
  - Interlock with the login overload gate and basic offload

Phase 3:
  - Introduce space artificialMinLoad
  - Interlock with loadBalance / updateRanges / retire / rollout
  - Form a complete runtime load feedback loop
```

---

## 12. Comparison with BigWorld / KBEngine / theseed

| Dimension | BigWorld | KBEngine | theseed |
|------|---------|---------|---------|
| EntityProfiler | Yes | Weak | Phase 2 goal |
| EntityTypeProfiler | Yes | Weak | Phase 2 goal |
| artificialMinLoad | Yes | Basically none | Explicitly modeled |
| overload duration gate | Yes | Weak | Should be explicitly modeled |
| profiler → balance feedback | Yes | Weak | Currently missing, filled in by this document |
