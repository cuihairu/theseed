# theseed Engine — Design Documentation Overview

> English | **中文**

> theseed's design origin is the server-side system areas and toolchain philosophy of BigWorld 14.4.1.
> KBEngine is one of the important reference implementations, mainly used to validate how the runtime loop can be implemented lightweight.
> The current goal is to integrate BigWorld's origin design with the KBEngine reference implementation to form an evolvable server engine for modern MMOs.
> The current basis does not count client rendering and WorldEditor UI details among the must-do items.

---

## Layering Principles

This round of restructuring is no longer read by the old flat feature directory, but by the engine's system areas:

```
0-foundation
  Engine positioning, baseline, audit scope, layering principles

1-runtime-model
  tick, entity, timer, runtime memory

2-replication-and-space
  AOI, Space, Ghost, Witness, Runtime Data Plane, migration, topology

3-cluster-and-availability
  fault tolerance, BackupHash, lifecycle, load feedback

4-data-and-ops
  data definition, primary persistence, local archive, data ops toolchain, server merge

5-access-and-control-plane
  gateway, login, message bus, ops control plane, telemetry/debug

6-world-and-game-framework
  physics, navigation, controllers, world streaming, lifecycle

7-scripting-and-client
  script security, hot update, script debugging, client SDK

8-reference
  source tracing and coverage judgment
```

For the audit scope and the reasons for the restructuring, see:

- [04-modern-mmo-engine-positioning](0-foundation/04-modern-mmo-engine-positioning.md)
- [02-audit-scope-and-layering](0-foundation/02-audit-scope-and-layering.md)

---

## Document Index

### 0-foundation

| Document | Contents |
|------|------|
| [01-mvp-architecture-baseline](0-foundation/01-mvp-architecture-baseline.md) | MVP scope, runtime boundaries, synchronization timing, migration and hot update constraints |
| [02-audit-scope-and-layering](0-foundation/02-audit-scope-and-layering.md) | Audit scope, BigWorld/KBEngine comparison, and new layering principles |
| [03-repository-and-build-layout](0-foundation/03-repository-and-build-layout.md) | monorepo directories, vcpkg, C++23 toolchain boundaries |
| [04-modern-mmo-engine-positioning](0-foundation/04-modern-mmo-engine-positioning.md) | BigWorld origin, KBEngine reference, theseed's modernization integration basis |

### 1-runtime-model

| Document | Contents |
|------|------|
| [01-tick-model](1-runtime-model/01-tick-model.md) | Single-threaded tick, phase timing, main loop constraints |
| [02-entity-system](1-runtime-model/02-entity-system.md) | Base/Cell dual-body model, PropertyBlock, lifecycle, object pool |
| [03-timer-and-memory](1-runtime-model/03-timer-and-memory.md) | TimerWheel, object pool, runtime memory management |
| [04-io-runtime-and-backend-abstraction](1-runtime-model/04-io-runtime-and-backend-abstraction.md) | IORuntime, Reactor/Completion backends, cross-platform I/O abstraction |

### 2-replication-and-space

| Document | Contents |
|------|------|
| [01-space-topology-and-aoi](2-replication-and-space/01-space-topology-and-aoi.md) | Space, AOI, topology phase model and configuration boundaries |
| [02-ghost-and-witness](2-replication-and-space/02-ghost-and-witness.md) | Witness view synchronization, Ghost real/ghost authority model |
| [03-runtime-communication-and-transport](2-replication-and-space/03-runtime-communication-and-transport.md) | EntityCall, Runtime Data Plane, transport semantics and reliability boundaries |
| [04-property-replication](2-replication-and-space/04-property-replication.md) | Property replication path, DirtyMask, sync build and flush |
| [05-entity-migration](2-replication-and-space/05-entity-migration.md) | Migration scenarios, in-window message handling, state constraints |
| [06-aoi-update-and-load-bounds](2-replication-and-space/06-aoi-update-and-load-bounds.md) | AoI update priority curve, Load Bounds, Range Update |
| [07-bsp-rebalance-and-offload](2-replication-and-space/07-bsp-rebalance-and-offload.md) | BSP grow/shrink, chunk-aware balance, offload |

### 3-cluster-and-availability

| Document | Contents |
|------|------|
| [01-fault-tolerance](3-cluster-and-availability/01-fault-tolerance.md) | Three-tier fault tolerance, recovery layering, MVP boundaries |
| [02-backup-hash-and-ha](3-cluster-and-availability/02-backup-hash-and-ha.md) | BackupHash, recovery chain, hot-standby switchover authority |
| [03-cluster-lifecycle](3-cluster-and-availability/03-cluster-lifecycle.md) | Service Fragment, Drain, Retire, Controlled Shutdown |
| [04-runtime-profiler-and-load-feedback](3-cluster-and-availability/04-runtime-profiler-and-load-feedback.md) | EntityProfiler, load feedback, overload determination, scheduling linkage |

### 4-data-and-ops

| Document | Contents |
|------|------|
| [01-data-definition](4-data-and-ops/01-data-definition.md) | XML + XSD definition specification, storage strategy, column type mapping |
| [02-persistence](4-data-and-ops/02-persistence.md) | Primary persistence, query interface split, online storage main path |
| [03-local-archive-and-secondary-db](4-data-and-ops/03-local-archive-and-secondary-db.md) | LocalArchiveStore, SecondaryDB, local archive staging layer |
| [04-data-ops-toolchain](4-data-and-ops/04-data-ops-toolchain.md) | Snapshot, Transfer, Consolidate, Sync, Repair |
| [05-server-merge](4-data-and-ops/05-server-merge.md) | Server merge scope, conflict repair, CLI tools and boundaries |

### 5-access-and-control-plane

| Document | Contents |
|------|------|
| [01-gateway-and-login](5-access-and-control-plane/01-gateway-and-login.md) | Access layer, login service, Challenge and login ops boundaries |
| [02-message-bus-and-cross-realm](5-access-and-control-plane/02-message-bus-and-cross-realm.md) | Control plane messages, cross-Realm async and bridging |
| [03-redis-async-and-config](5-access-and-control-plane/03-redis-async-and-config.md) | Redis, async task framework, unified configuration |
| [04-ops-control-plane](5-access-and-control-plane/04-ops-control-plane.md) | Distributed observation, online commands, config hot change, audit |
| [05-telemetry-and-debug](5-access-and-control-plane/05-telemetry-and-debug.md) | Telemetry, Debug, Diagnostics and OTel boundaries |
| [06-machine-agent-and-host-ops](5-access-and-control-plane/06-machine-agent-and-host-ops.md) | machine / host ops / node summary / resource reporting |

### 6-world-and-game-framework

| Document | Contents |
|------|------|
| [01-physics](6-world-and-game-framework/01-physics.md) | Server-side physics queries, IPhysicsQuery interface |
| [02-navigation](6-world-and-game-framework/02-navigation.md) | INavigationSystem, NavMesh and Space binding |
| [03-controllers](6-world-and-game-framework/03-controllers.md) | Behavior controllers, ControllerManager, script API |
| [04-world-streaming-and-compiled-space](6-world-and-game-framework/04-world-streaming-and-compiled-space.md) | Chunk, Geometry Mapping, Compiled Space boundaries |
| [05-built-in-entities](6-world-and-game-framework/05-built-in-entities.md) | Built-in game objects and framework boundaries |
| [06-lifecycle-and-script-binding](6-world-and-game-framework/06-lifecycle-and-script-binding.md) | Lifecycle hooks, script binding architecture, def-driven automatic binding |

### 7-scripting-and-client

| Document | Contents |
|------|------|
| [01-script-security](7-scripting-and-client/01-script-security.md) | Script layer security, source protection, runtime resilience |
| [02-hot-update](7-scripting-and-client/02-hot-update.md) | L1-L4 tiered hot update, verification process, rolling update |
| [03-script-debug](7-scripting-and-client/03-script-debug.md) | Script breakpoints, execution stack, Entity context, yield/resume, version consistency |
| [04-client-sdk](7-scripting-and-client/04-client-sdk.md) | Code generation, Unity/UE5/Cocos SDK, property interpolation |

### 8-reference

| Document | Contents |
|------|------|
| [source-attribution](8-reference/source-attribution.md) | Source tracing, coverage judgment, and BigWorld/KBEngine comparison |

---

## Reading Suggestions

**Runtime loop**

`0-foundation/04-modern-mmo-engine-positioning`
→ `0-foundation/01-mvp-architecture-baseline`
→ `1-runtime-model/01-tick-model`
→ `1-runtime-model/02-entity-system`
→ `2-replication-and-space/01-space-topology-and-aoi`
→ `2-replication-and-space/03-runtime-communication-and-transport`

**BigWorld origin system areas**

`0-foundation/04-modern-mmo-engine-positioning`
→ `3-cluster-and-availability/02-backup-hash-and-ha`
→ `4-data-and-ops/03-local-archive-and-secondary-db`
→ `4-data-and-ops/04-data-ops-toolchain`
→ `5-access-and-control-plane/01-gateway-and-login`
→ `5-access-and-control-plane/04-ops-control-plane`

**Business framework**

`6-world-and-game-framework/06-lifecycle-and-script-binding`
→ `6-world-and-game-framework/03-controllers`
→ `4-data-and-ops/01-data-definition`
→ `7-scripting-and-client/01-script-security`
→ `7-scripting-and-client/03-script-debug`

**Diagnostics and ops**

`5-access-and-control-plane/05-telemetry-and-debug`
→ `3-cluster-and-availability/04-runtime-profiler-and-load-feedback`
→ `7-scripting-and-client/02-hot-update`
→ `7-scripting-and-client/03-script-debug`
→ `3-cluster-and-availability/03-cluster-lifecycle`

**Repository and toolchain**

`0-foundation/03-repository-and-build-layout`
→ `0-foundation/01-mvp-architecture-baseline`
→ `1-runtime-model/04-io-runtime-and-backend-abstraction`

**Host ops**

`5-access-and-control-plane/06-machine-agent-and-host-ops`
→ `5-access-and-control-plane/04-ops-control-plane`
→ `5-access-and-control-plane/05-telemetry-and-debug`
