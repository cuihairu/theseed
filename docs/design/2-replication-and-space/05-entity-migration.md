# Entity Migration — Entity Migration

> **English** | [中文](05-entity-migration.zh.md)

> Entity migration is a core capability of theseed's distributed architecture: entities can migrate safely between different CellApps.
>
> Origin: BigWorld Offload / Cell migration / retire-offload mechanisms.
> Reference implementation: KBEngine teleport + Ghost routing window.
> The current implementation baseline is defined by [0-foundation/01-mvp-architecture-baseline](../0-foundation/01-mvp-architecture-baseline.md).

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
BigWorld's migration is not a simple teleport:
  - It has offload / retire / controlled shutdown semantics
  - It has a cross-Cell migration window
  - It has a more complete load balancing and retirement chain
```

### How KBEngine Implements It

```
KBEngine leans more toward teleport and the ghost routing window:
  - The migration path is more direct
  - The system-level retirement chain is lighter
  - Suitable for first getting entity migratability working
```

### Pros and Cons

```
BigWorld's advantages:
  - Complete migration and retirement system
  - Can support more complex cluster scheduling

KBEngine's advantages:
  - Simple
  - Easy to land first

Common disadvantages:
  - Both the migration snapshot and the window-period control are sensitive
  - Once tightly bound to script I/O and coroutines, complexity rises quickly
```

### theseed's Trade-offs

```
theseed first implements intra-Realm migration and cross-Space teleport,
explicitly excludes the script call stack and external I/O from the snapshot,
avoiding swallowing BigWorld's HA complexity into the MVP all at once.
```

---

## 0. MVP Boundary

```
The MVP only supports:
  - Intra-Realm Cell migration
  - Cross-Space teleport

The MVP does not support:
  - Cross-datacenter / cross-Realm entity migration
  - Script call stack migration
  - Suspended coroutine / Future continuation migration
  - External I/O context migration
```

---

## 1. Migration Scenarios

```
Scenario 1: Teleport (cross-Space teleport)
  A player teleports from Space A to Space B
  May cross CellApps

Scenario 2: Cell boundary movement
  A Space topology change causes the Cell boundary to move
  The entity needs to migrate to a new CellApp

Scenario 3: Load balancing
  A CellApp is overloaded, and some entities migrate to other CellApps

Scenario 4: Cross-server (long-term)
  An entity migrates from one Realm to another
  Not within MVP scope
```

---

## 2. Migration Flow

```
Phase 1: Prepare
  ├─ Source CellApp: freeze the entity's inbox
  ├─ Mark the entity epoch / migration token
  ├─ Set up the GhostManager route (forward messages during the window period)
  └─ Notify the target CellApp to prepare to receive

Phase 2: Serialize
  ├─ Serialize the entity state:
  │   - PropertyBlock (persistence + runtime-necessary properties)
  │   - Position / orientation / Space information
  │   - Timer list
  │   - Controller state
  │   - Witness base state
  └─ Send to the target CellApp

  Not included in the migration snapshot:
    - Python / Lua call stacks
    - Continuations suspended at yield
    - In-flight HTTP / DB / cross-server request contexts

Phase 3: Restore
  ├─ Target CellApp: create a new entity
  ├─ Deserialize properties
  ├─ Restore timers
  ├─ Add to the Space / CoordinateSystem
  ├─ Restore the Witness (if there is a client)
  └─ Establish a new entity epoch

Phase 4: Route Update
  ├─ Update the EntityCall route: old CellApp → new CellApp
  ├─ Notify BaseApp to update the cellEntityCall target
  ├─ GhostManager route window period: forward runtime messages to the new location
  └─ Notify known holders to update the address

Phase 5: Cleanup
  ├─ Old CellApp: destroy the old entity (callScript=false)
  ├─ Clean up the GhostManager route
  └─ Discard late callbacks from the old epoch
```

---

## 3. Message Handling During the Migration Window

```
Problem: during migration, runtime messages may be sent to the old CellApp

KBEngine's solution (from source comments):
  "If a base message is sent over during this period, the entity's ghost mechanism can forward it to the real"

theseed's design:
  GhostManager.setRoute(target, ttl=10s)
  → During the route window period, the old CellApp forwards runtime messages to the new location
  → After the route expires, if messages still arrive, it means the routing table has not finished updating; record an alert

Coverage:
  - EntityCall
  - Ghost / Witness synchronization
  - Runtime control messages during the migration window

Not covered:
  - Already-dispatched external HTTP requests
  - Already-submitted DB async requests
  - The remote execution context of cross-Realm async tasks

Therefore:
  The goal of the migration window mechanism is "seamless switching of runtime messages",
  not an absolute zero-loss guarantee for all asynchronous side effects.
```
