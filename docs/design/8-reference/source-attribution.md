# Source Attribution — Origin Traceability and Coverage Judgment

> **English** | [中文](source-attribution.zh.md)

> This document answers three things:
>
> 1. Under the new layering, for each layer, whether it comes mainly from the BigWorld origin, the KBEngine reference implementation, or theseed's own extension  
> 2. Whether the current documentation covers the "BigWorld origin system plane" or the "KBEngine lightweight runtime reference"  
> 3. Which capabilities are currently only boundaries, not MVP commitments

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Is Implemented

```
BigWorld is theseed's origin design reference,
its value lies not only in the runtime, but even more in the system layer and toolchain:
  - Space / AOI / Witness / Ghost
  - HA
  - data ops
  - login ops
  - Watcher / profiler / controlled shutdown
```

### How KBEngine Is Implemented

```
KBEngine is one of the important reference implementations,
its reference value lies mainly in how a lightweight runtime is put into practice:
  - tick
  - entity
  - AOI / ghost / witness
  - migration
```

### Strengths and Weaknesses

```
BigWorld's strengths:
  - a substantial system area
  - can serve as the origin of theseed's long-term system boundary

KBEngine's strengths:
  - the runtime can be implemented more directly
  - better suited as the reference for the MVP runtime chain

Shared weaknesses:
  - without origin traceability, it is easy for the documentation to misjudge “what is already covered”
```

### theseed's Trade-offs

```
The task of this origin-traceability document for theseed
is to keep reminding the reader:
  - which parts come from the BigWorld origin design
  - which parts come from the KBEngine reference implementation
  - which parts are new in theseed
  - which parts are only boundaries, not current commitments
```

---

## 1. Overall Judgment

In one sentence:

```
theseed is not a rewrite of KBEngine,
nor an item-by-item port of BigWorld.

theseed's documentation structure should take BigWorld's MMO server system area as its origin,
take the KBEngine lightweight runtime as its reference,
and then make theseed's own modern engineering trade-offs explicit.
```

But two layers must be distinguished:

```
MVP-implementable starting point
  - draw on the KBEngine lightweight runtime reference implementation

Long-term system boundary
  - explicitly inherit BigWorld's HA, data ops, login ops, control plane, load feedback, and world system
```

---

## 2. Newly Added Infrastructure Boundaries

### Repository Layout and Build

```
The monorepo / vcpkg / C++23 constraint set is mainly theseed's own design choice;
BigWorld and KBEngine only provide a reference for organizational form, not a ready-made modern repository model.
```

### MachineAgent / Host Ops

```
The reference origin for this part is BigWorld bwmachined + KBEngine machine.
theseed's convergence is:
  - keep the node agent
  - keep local process orchestration
  - keep node summary reporting
  - but split out the global control plane and the diagnosis plane
```

---

## 3. Layer-by-Layer Origins

### 3.1 0-foundation

Main origin:

```
Defined by theseed itself
```

Note:

```
The audit scope, layering principles, and MVP baseline
are not ready-made documentation structures provided directly by BigWorld / KBEngine,
but a governance layer added in this round of reorganization to prevent boundary confusion.
```

### 3.2 1-runtime-model

| Topic | Main origin | Note |
|------|------|------|
| Tick 5-phase segmentation | KBEngine | The `EventDispatcher::processUntilBreak` style is the clearest |
| Base / Cell dual body | BigWorld | KBEngine provides a lighter inheritance implementation |
| IORuntime / Reactor-Completion layering | theseed | The old engines only have event-loop / poller semantics, without this overall abstraction |
| Merkle diff / semantic compatibility | theseed | The old engines mostly do single-digest comparison, lacking layered difference localization |
| PropertyBlock contiguous memory | theseed | Used to replace the Python dict property layout |
| DirtyMask bitmap | KBEngine idea + theseed implementation | The dirty-flag concept comes from existing engines; the bitmap form is theseed's convergence |
| TimerWheel | General algorithm + theseed selection | Not native to BigWorld / KBEngine |
| Object pool pattern | KBEngine | Continuation of patterns like `OBJECTPOOL_POINT` |

Judgment:

```
This layer is “BigWorld entity model + KBEngine main-loop decomposition + theseed memory redesign”.
```

### 3.3 2-replication-and-space

| Topic | Main origin | Note |
|------|------|------|
| AOI cross-linked list / RangeTrigger | BigWorld | KBEngine provides a lightweight inheritance implementation |
| Witness / Ghost / real-ghost authority | BigWorld | KBEngine inherits but simplifies |
| EntityCall / Mailbox idea | BigWorld | KBEngine made a one-way simplification |
| Migration window and routing period | KBEngine | The GhostManager routing window is expressed more directly |
| Runtime Data Plane does not go through MQ | theseed | An explicit judgment based on the direct-connection mode of both engines |
| AoI update scheme / load bounds | BigWorld | KBEngine basically has no equivalent system plane |
| BSP rebalance / offload | BigWorld | The key layer that sets theseed apart from KBEngine |

Judgment:

```
This layer is currently the most “BigWorld-aligned” core layer.
```

### 3.4 3-cluster-and-availability

| Topic | Main origin | Note |
|------|------|------|
| Reviver / supervisor idea | BigWorld | KBEngine is weaker |
| BackupHash / BackupHashChain | BigWorld | KBEngine is basically missing it |
| Service Fragment / Retire / Controlled Shutdown | BigWorld | KBEngine has no systematic implementation |
| EntityProfiler → load feedback | BigWorld | A system-level differentiator of BigWorld |
| The current MVP keeps only the boundary | theseed | Does not disguise long-term HA as a current capability |

Judgment:

```
This layer draws its boundary almost entirely from the BigWorld server system layer.
```

### 3.5 4-data-and-ops

| Topic | Main origin | Note |
|------|------|------|
| Property-level storage mapping idea | BigWorld | More complete than KBEngine's BLOB-ification |
| The realistic starting point of the MySQL main path | KBEngine | More suitable for an implementable MVP |
| LocalArchiveStore / SecondaryDB semantics | BigWorld | The focus is responsibility, not a specific backend |
| snapshot / transfer / consolidate / sync / repair | BigWorld toolchain | `transfer_db / consolidate_dbs / sync_db` |
| JSON, multi-backend, Schema auto-migration | theseed | A modern extension, not a copy of the old engines |

Judgment:

```
This layer is “BigWorld toolchain boundary + theseed modern storage enhancements”.
```

### 3.6 5-access-and-control-plane

| Topic | Main origin | Note |
|------|------|------|
| Login Challenge / statusCheck / ban cleanup | BigWorld | The most important system reference for the login plane |
| LoginApp / Gateway access baseline | BigWorld + KBEngine | The old engines both only provide a thin access plane |
| MessageBus / Cross-Realm | theseed | Neither engine has a true modern MQ control plane |
| Redis / Async / Config | KBEngine basics + theseed extension | KBEngine only provides basic support |
| Watcher-style control plane abstraction | BigWorld | But the implementation stack is redesigned by theseed itself |
| OTel / Telemetry / Diagnostics | theseed | A modern observability extension; script runtime debugging is split out separately |

Judgment:

```
This layer is not simply a “cloud-native add-on”,
but puts the BigWorld login ops plane and the Watcher control plane back into the system boundary.
```

### 3.7 6-world-and-game-framework

| Topic | Main origin | Note |
|------|------|------|
| Physics / Navigation / Controllers | General design + theseed | Both engines are referenceable but are not the same abstraction |
| World Streaming / Compiled Space | BigWorld | An important completion of the BigWorld world system |
| Built-in Entities | theseed | More on the game-framework side |
| Lifecycle / Script Binding | BigWorld / KBEngine ideas + theseed convergence | The hook table and auto-binding are the result of tidying up |

Judgment:

```
This layer is mainly “world system boundary re-homing”,
so these systems are no longer lumped together generically as gameplay.
```

### 3.8 7-scripting-and-client

| Topic | Main origin | Note |
|------|------|------|
| Basic hot-update idea | KBEngine reload + theseed extension | The L1-L4 grading is defined by theseed itself |
| Exposed security boundary | KBEngine | Continuation of the client-call authorization boundary |
| Script runtime debugging | theseed | BigWorld / KBEngine mainly have traceback, watcher, and reload, not systematic breakpoint debugging |
| Multi-platform code generation / Unity first | theseed | Neither of the two old engines has a modern codegen structure |
| Script security tiers | theseed | Almost entirely new |

Judgment:

```
This layer inherits less; it is mostly theseed's own productization layer.
```

---

## 4. Coverage Conclusion

### 4.1 With BigWorld as the Origin Yardstick

The current documentation already explicitly covers the design boundaries:

```
  - Space / AOI / Witness / Ghost / EntityCache
  - Channel / Bundle / Runtime Data Plane
  - BackupHash / hot-standby recovery chain
  - SecondaryDB / LocalArchiveStore
  - Data Ops Toolchain
  - Login ops plane
  - Watcher-style control plane
  - Runtime profiler → scheduling feedback
  - BSP / Load Bounds / Offload
  - World Streaming / Compiled Space
```

But it must be emphasized:

```
Many of these are still Phase 2 / Phase 3 boundaries,
not MVP implementation commitments.
```

### 4.2 Using the KBEngine Reference Implementation to Validate the MVP Runtime Chain

The current documentation already covers:

```
  - Single-threaded tick main loop
  - Base / Cell
  - AOI / Witness / Ghost
  - EntityCall
  - Property replication
  - Single-Realm persistence main path
  - Unity-first client main chain
```

So if the goal is:

```
building a runtime core that is clearer than KBEngine's, with more modern boundaries
```

The current documentation is already sufficient.

## 5. Current Capability Assessment

| Capability area | Coverage status | Note |
|------|------|------|
| Base / Cell / Tick / AOI / Ghost / Witness | Covered | Can serve as the MVP runtime chain |
| EntityCall / Property Replication / Migration | Covered | The metric basis has been unified into the replication-and-space layer |
| SingleCell / Static Partition boundary | Covered | Phase 1 / 2 are clear |
| BigWorld-level BSP / grow-shrink / auto rebalance | Boundary defined | Not MVP |
| Main persistence / data definition / query | Covered | Implementable in the MVP |
| LocalArchiveStore / SecondaryDB | Boundary defined | Not MVP |
| BackupHash / hot-standby recovery chain | Boundary defined | Not MVP |
| Login Challenge / login ops plane | Boundary covered | A BigWorld alignment point |
| Watcher / control plane / online commands | Boundary covered | No longer mixed into OTel |
| Runtime profiler → scheduling feedback loop | Boundary covered | A key BigWorld differentiator |

---

## 6. Summary

The greatest contributions that truly come from the BigWorld origin design are mainly in four layers:

```
2-replication-and-space
3-cluster-and-availability
4-data-and-ops
6-world-and-game-framework
```

The realistic implementation value that truly comes from the KBEngine reference implementation is mainly in two layers:

```
1-runtime-model
and the MVP starting form of 2-replication-and-space
```

The layers where theseed itself adds the most are:

```
5-access-and-control-plane
7-scripting-and-client
```

But the key change after this round of reorganization is:

```
These new capabilities no longer float adrift in the old directories,
but have been put back into more sensible system layers, with BigWorld as the origin and KBEngine as the reference.
```
