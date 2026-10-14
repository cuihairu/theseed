# Modern MMO Engine Positioning — Positioning a Modern MMO Engine

> **English** | [中文](04-modern-mmo-engine-positioning.zh.md)

> This document is used to unify theseed's top-level trade-offs: BigWorld is the origin design, KBEngine is one of the reference implementations, and theseed's goal is to integrate the strengths of both and deliver it in a modernized way.

---

## 1. Overall Judgment

theseed should be positioned as:

```
A distributed game server engine that takes BigWorld's system-level MMO server architecture as its origin,
takes KBEngine's lightweight runtime implementation as a reference,
and is reorganized with modern C++, modern ops, and modern data and toolchain.
```

This means:

```
BigWorld sets the upper bound of system boundaries.
KBEngine helps validate the minimal runtime loop.
theseed is responsible for modernization, engineering, and staged implementation.
```

---

## 2. Three Reference Levels

### 2.1 BigWorld: origin design

BigWorld's value is not just the terms Base / Cell / AOI, but the complete system areas:

```
  - Runtime Data Plane: Mailbox / Mercury / Channel / Bundle
  - Replication & Space: Space / Cell / Witness / Ghost / EntityCache
  - Cluster & Availability: BackupHash / retire / controlled shutdown
  - Data & Ops: SecondaryDB / transfer / consolidate / sync / repair
  - Control & Diagnostics: Watcher / profiler / status / load feedback
  - World Framework: chunk / geometry mapping / compiled space
```

Together these capabilities form the system skeleton of a "large-scale MMO server engine".

### 2.2 KBEngine: one of the reference implementations

KBEngine's value lies in:

```
  - A more readable runtime backbone
  - Lighter Base / Cell / EntityCall / Witness / Ghost paths
  - Usable as a realistic reference for the MVP runtime chain
```

But KBEngine should not become theseed's capability upper bound:

```
  - System-level HA is weaker
  - The data ops toolchain is thinner
  - load feedback and the scheduling loop are insufficient
  - The ops control plane is less systematic than BigWorld's
```

### 2.3 theseed: modernization integration

theseed needs to reorganize the correct system semantics of the old engines into a modern engineering structure:

```
  - A modern build baseline of C++23 / CMake / vcpkg
  - Explicit separation of the Runtime Data Plane / Control Plane / Cross-Realm Async Plane
  - Layering of structured logging, metrics, trace, and the Ops Control Plane
  - Modern data capabilities of property-level storage mapping + JSON / multiple backends / schema diff
  - Code-generation-first client SDK and protocol constraints
  - Engineering release boundaries of restricted hot update, rolling release, audit, and rollback
```

---

## 3. Trade-off Principles

### 3.1 Keep BigWorld's system state objects

Modernization does not mean erasing BigWorld's core state objects. theseed should preserve these semantics first:

| BigWorld concept | Semantics theseed needs to preserve |
|------|------|
| Channel | Connection state, backpressure, inactivity, reliability context |
| Bundle | In-tick message aggregation, reliability markers, flush boundary |
| RealEntity / Haunt | real authority, ghost target, control ownership, migration/offload state |
| Witness / EntityCache | Per-observation-relationship visibility, detail level, priority, bandwidth budget |
| EntityGhostMaintainer | ghost set maintenance and cross-Cell boundary consistency |
| BackupHash / BackupHashChain | Backup responsibility routing, priming, ack, promote, recovery authority |
| EntityProfiler / EntityTypeProfiler | Load signals entering the scheduling loop, rather than being display metrics only |

These objects need not be copied in their old code form, but the runtime semantics they carry must not be lost.

### 3.2 Adopt KBEngine's minimal loop path

The MVP should remain lightweight:

```
  - Single Realm
  - Single-threaded tick
  - SingleCell Space
  - Base / Cell dual bodies
  - EntityCall main path
  - Witness synchronization
  - A minimal routing window for Ghost / migration
```

But this lightweight path is only a starting point, not the final boundary.

### 3.3 Use modernization to split the old engines' mixed layers

Many capabilities in the old engines are correct, but they are often mixed together in implementation. theseed should explicitly split them apart:

| Content that should not be mixed | theseed layering |
|------|------|
| Entity main-path messages and ops commands | Runtime Data Plane / Ops Control Plane |
| metrics display and control commands | Telemetry / Ops Control Plane |
| profiler sampling and scheduling decisions | Diagnostics / Load Feedback |
| visibility, update priority, payload encoding | AOI / Priority / Property Replication |
| storage main path and offline data ops | Persistence / Data Ops Toolchain |

---

## 4. Core Chains of a Modern MMO Engine

theseed's long-term architecture should be organized around the following chains:

```
Login / Gateway
  → Base Entity
  → Cell Entity
  → Space / AOI
  → Witness / Ghost
  → Runtime Transport
  → Client SDK
```

And build the system chain in parallel:

```
Runtime Profiler
  → Load Feedback
  → Topology / Offload / Throttle
  → Ops Control Plane
```

As well as the availability chain:

```
Primary Entity
  → Backup Route
  → Backup Snapshot
  → HA Coordinator
  → Restore / Promote
```

These three chains correspond respectively to:

```
The player online main path
The system scheduling feedback path
The high-availability recovery path
```

Any document or implementation that covers only the first chain cannot yet be called a complete MMO engine.

---

## 5. Staged Goals

### Phase 1: Modernized runtime kernel

```
  - Base / Cell / Tick / EntityDef
  - Runtime Data Plane
  - SingleCell Space
  - AOI / Witness
  - Property Replication
  - Gateway MVP
  - MySQL MVP
  - Unity SDK MVP
```

The goal is to obtain a single-Realm distributed game server kernel that runs stably.

### Phase 2: Completing the system areas

```
  - Static Partition
  - A more complete Channel / Bundle state machine
  - EntityCache / witness priority
  - Entity / EntityType load profiler
  - Ops Control Plane
  - BackupTopologyCoordinator prototype
  - Data Ops Toolchain prototype
```

The goal is to move from "it runs" to "operable, scalable, schedulable".

### Phase 3: Modernizing BigWorld-level system capabilities

```
  - BSP grow / shrink
  - Offload / retire / controlled shutdown
  - BackupHash / restore authority loop
  - load bounds / chunk-aware balance
  - SecondaryDB / local archive / consolidate
  - Multi-Realm / Cross-Realm Async Plane
```

The goal is to form system-level engine capabilities truly oriented toward large-scale MMOs.

---

## 6. In One Sentence

```
theseed is not a rewrite of KBEngine,
nor an item-by-item port of BigWorld.

theseed should be an MMO server engine that takes BigWorld as its origin, takes KBEngine as one of its references,
and is redesigned and implemented in a modern engineering way.
```
