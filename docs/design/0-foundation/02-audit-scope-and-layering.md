# Audit Scope & Layering — Audit Scope and Layering Principles

> **English** | [中文](02-audit-scope-and-layering.zh.md)

> This round of document restructuring is benchmarked against the server-side implementation and server-side toolchain of BigWorld 14.4.1,
> while referencing KBEngine's lightweight runtime implementation.
> See [04-modern-mmo-engine-positioning](04-modern-mmo-engine-positioning.md) for the top-level positioning.
> Client rendering, resource editor UI, and WorldEditor interaction details are not included in the current must-do items.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld implements it

```
BigWorld is the origin reference for these system boundaries:
  - runtime backbone
  - Space / AOI / Witness / Ghost
  - HA
  - data ops
  - login and control plane
  - world assets and toolchain
```

### How KBEngine implements it

```
KBEngine leans more toward a lightweight runtime loop:
  - tick / entity / AOI / witness / migration
  - the system layer and toolchain are relatively thinner
```

### Pros and cons

```
BigWorld's pros:
  - Complete system areas
  - Can directly support heavier ops and live-ops capabilities

KBEngine's pros:
  - Light
  - Better suited as a reference implementation for the runtime loop

Shared cons:
  - If the audit scope is not established first, it is very easy to mix "long-term boundaries" with "current implementation"
```

### theseed's trade-offs

```
theseed takes BigWorld's system layering as its long-term boundary,
validates the MVP path against KBEngine's lightweight runtime reference,
and then places modern infrastructure back into explicit layers.
In this way the documents neither underestimate BigWorld's origin value
nor mistake KBEngine's simplified implementation for the capability upper bound.
```

---

## 0. Audit Scope

The audit scope of the current documents is fixed as:

```
In scope:
  - Runtimes of the BaseApp / CellApp / BaseAppMgr / CellAppMgr kind
  - LoginApp / Gateway / Challenge / controlled shutdown
  - AOI / Witness / Ghost / EntityCall / Migration
  - BackupHash / SecondaryDB / data ops toolchain
  - Watcher / Profiler / ops control plane
  - Server-side asset boundaries of Chunk / Compiled Space

Not included in the current must-do items:
  - Client rendering
  - WorldEditor interaction UI details
  - Full implementation of art production tools
```

Therefore what this document set must answer is not:

```
How to build a complete MMO editor ecosystem
```

but rather:

```
As a distributed game server engine,
which system areas theseed should cover within the capability landscape of BigWorld / KBEngine,
and which are merely boundary placeholders for Phase 2 / Phase 3.
```

---

## 1. Why Reorganize by System Area

The main problem with the old directory was not "ugly file names" but mixed levels of abstraction:

```
The old runtime/core directory
  mixed runtime model, replication, topology, and HA together

The old infrastructure directory
  mixed access plane, cluster lifecycle, and runtime feedback together

The old gameplay directory
  actually contained a large amount of world / space / chunk / server framework content

The old observability directory
  an old directory name, already split into the three boundaries of telemetry, ops control plane, and script debug
```

This leads to two consequences:

```
1. BigWorld's system-level implementation areas are broken apart, making the complete boundary invisible
2. KBEngine's lightweight runtime reference and the modern infrastructure theseed added itself are tangled together
```

So this round of restructuring adopts a new criterion:

```
Classify not by "feature scatter points",
but by "engine system areas".
```

---

## 2. New Layering Principles

### 2.1 0-foundation

Responsible for:

```
  - MVP baseline
  - Audit scope
  - Document layering principles
```

### 2.2 1-runtime-model

Responsible for:

```
  - tick model
  - entity basic object model
  - timer / object pool / runtime memory
```

This layer answers only:

```
How the engine runs correctly within a single tick thread.
```

### 2.3 2-replication-and-space

Responsible for:

```
  - AOI / Space / Topology
  - Ghost / Witness
  - Runtime Data Plane
  - Property Replication
  - Entity Migration
  - AoI Update / Load Bounds
  - BSP Rebalance / Offload
```

This layer corresponds to the most core runtime distributed capabilities of BigWorld / KBEngine.

### 2.4 3-cluster-and-availability

Responsible for:

```
  - fault tolerance
  - BackupHash / HA
  - service fragment / retire / controlled shutdown
  - profiler → scheduling feedback loop
```

This layer mainly benchmarks the system layer where BigWorld truly pulls ahead.

### 2.5 4-data-and-ops

Responsible for:

```
  - data definition
  - persistence
  - local archive / secondary db
  - snapshot / transfer / consolidate / sync / repair
  - server merge
```

This layer clearly separates the "online storage main path" from the "offline data ops toolchain".

### 2.6 5-access-and-control-plane

Responsible for:

```
  - gateway / login service
  - message bus / cross-realm async plane
  - redis / async / config
  - ops control plane
  - telemetry / debug / diagnostics
```

The focus here is:

```
The access plane, control plane, and observation plane are on the outer edge of the system,
but in BigWorld they are likewise part of the server-side capability boundary.
```

### 2.7 6-world-and-game-framework

Responsible for:

```
  - physics / navigation
  - controller framework
  - world streaming / compiled space
  - built-in entities
  - lifecycle / script binding
```

This layer is no longer called gameplay because it contains a large amount of "world system" and "server-side game framework".

### 2.8 7-scripting-and-client

Responsible for:

```
  - script security
  - hot update
  - script debug
  - client sdk / codegen
```

### 2.9 8-reference

Responsible for:

```
  - Source tracing
  - Coverage judgment
  - Explanation of the differences between BigWorld / KBEngine / theseed
```

---

## 3. Mapping to BigWorld / KBEngine

| System area | Main BigWorld source | Main KBEngine source | theseed document layer |
|------|------|------|------|
| Tick / Entity / Runtime Memory | BaseApp / CellApp runtime | EventDispatcher / Entity | 1-runtime-model |
| AOI / Witness / Ghost / Migration | CellApp AOI / Ghost / Mailbox | AOI / Witness / EntityCall | 2-replication-and-space |
| BSP / Load Bounds / Offload | CellAppMgr / BSP / load balancing | basically no complete system | 2-replication-and-space |
| HA / BackupHash / Retire | BaseAppMgr / BackupSender / Service Fragment | relatively weak | 3-cluster-and-availability |
| SecondaryDB / DB Tools | DBMgr / SecondaryDB / transfer_db | the persistence main path is fairly simplified | 4-data-and-ops |
| Login / Watcher / Status Check | LoginApp / Watcher / message_logger | simplified version of LoginApp / Watcher | 5-access-and-control-plane |
| Space Asset / Chunk / Compiled Space | Geometry Mapping / Chunk / Compiled Space | basically no complete equivalent layer | 6-world-and-game-framework |
| Script / Client | Python integration / client protocol | Python + client SDK | 7-scripting-and-client |

---

## 4. Current Coverage Judgment

Checked against the KBEngine lightweight reference implementation:

```
The runtime loop has basically reached a state that is "designable and implementable".
```

Checked against BigWorld's origin system areas:

```
These documents no longer cover only the runtime core,
but also establish the system boundaries of HA, data ops, login ops, control plane, and load feedback.
```

But staging must still be made explicit:

| Capability domain | Current status |
|------|------|
| Base / Cell / Tick / AOI / Ghost / Witness | Covered |
| SingleCell / Static Partition | Covered |
| BigWorld-level BSP grow-shrink / auto rebalance | Boundary defined, not MVP |
| Primary persistence and data definition | Covered |
| LocalArchiveStore / SecondaryDB | Boundary defined, not MVP |
| BackupHash / hot-standby recovery chain | Boundary defined, not MVP |
| Login ops plane / Challenge lifecycle | Design boundary covered |
| Watcher-style control plane | Design boundary covered |
| Runtime profiler → scheduling feedback loop | Design boundary covered |

---

## 5. Suggested Reading Order

### 5.1 Start with the runtime loop

```
01-mvp-architecture-baseline
  → 1-runtime-model
  → 2-replication-and-space
```

### 5.2 Then the BigWorld origin system areas

```
3-cluster-and-availability
  → 4-data-and-ops
  → 5-access-and-control-plane
```

### 5.3 Finally the game framework and client boundaries

```
6-world-and-game-framework
  → 7-scripting-and-client
```

---

## 6. Current Positioning

After this round of reorganization, the theseed documents should be understood as:

```
Taking the server-side system areas of BigWorld 14.4.1 as the origin boundary,
taking KBEngine's lightweight runtime implementation as one of the reference implementations,
and at the same time placing the modern infrastructure theseed added itself explicitly back into the appropriate layers.
```

This is closer to a truly auditable, evolvable engine document structure than "Runtime Core + a pile of extensions".
