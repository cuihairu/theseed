# MVP Architecture Baseline — An Implementable Architecture Baseline

> English | **中文**

> This document does not replace the existing per-topic designs; it provides unified constraints for the current implementation stage.
> The goal is to first build a theseed MVP that runs stably, has clear boundaries, and can evolve continuously.
> See [04-modern-mmo-engine-positioning](04-modern-mmo-engine-positioning.md) for the top-level positioning.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld implements it

```
BigWorld is theseed's origin reference:
  - Strong Runtime backbone
  - Complete Space / AOI / Witness / Ghost semantics
  - HA / data / login / control plane / toolchain form a coherent system
  - But heavy overall, so the MVP cannot port it item by item
```

### How KBEngine implements it

```
KBEngine is one of theseed's important reference implementations:
  - Concise tick / entity / AOI / EntityCall
  - Strong runtime implementation
  - Good for validating how the minimal loop runs end to end
  - But its system-layer capabilities are thin
```

### Pros and cons

```
BigWorld pros:
  - Complete system boundaries
  - Runtime and ops chains form a coherent system

BigWorld cons:
  - Heavy
  - Complex
  - High cost to port directly into the MVP

KBEngine pros:
  - Light
  - Easy to implement
  - A realistic reference for the runtime loop

KBEngine cons:
  - Thin system-level capabilities
  - Ops, HA, and the toolchain must be built yourself
```

### theseed's trade-offs

```
theseed takes BigWorld's MMO server-side system areas as its origin boundary,
takes KBEngine's lightweight runtime implementation as one of its reference implementations,
and then re-splits, converges, and implements it in a modern engineering way.

Therefore the MVP targets a runnable loop,
while not treating KBEngine's simplified capabilities as an upper bound.
The current stage implements only the minimal runnable loop,
but the document boundaries must preserve the direction of BigWorld-level system capabilities.
```

---

## 1. Purpose of This Document

The current design documents cover a very wide range, already including:

- Base/Cell, Ghost/Witness, AOI, migration, fault tolerance
- XML/XSD data definition, multi-backend storage, server merge
- Gateway, message bus, Redis, async tasks
- Hot update, client SDK, OTel, script debugging, auto-scaling

These directions are fine in themselves, but before entering the implementation stage, the following must be clarified first:

1. Which capabilities belong to the MVP
2. Which capabilities are only reserved for Phase 2 / Phase 3
3. What the hard boundaries between modules are
4. What the runtime consistency rules are

This document addresses these 4 questions first.

---

## 2. MVP Goals

theseed's MVP does not aim to "fully replace BigWorld", nor does it target "redoing KBEngine".
The MVP pursues only one thing:

**Build a distributed game server kernel that runs within a single datacenter, is single-Realm, and can support room-based/instance-based/small-to-medium map gameplay.**

It must have the structural headroom to keep evolving into a large MMO, but must not stuff the system complexity of Phase 2 / Phase 3 into Phase 1 ahead of time.

The MVP explicitly supports:

- Base/Cell dual-body model
- Single-threaded tick runtime
- SingleCell Space
- Cross-linked list AOI
- Witness view synchronization
- Base ↔ Cell EntityCall
- MySQL persistence
- XML/XSD data definition
- Gateway access
- Metrics + Logging
- Unity client code generation

The MVP explicitly does not support:

- Dynamic multi-Cell topology
- Cross-datacenter cross-Realm migration
- Script stack migration
- L3/L4 hot update
- Multiple database backends being fully available at once
- Full engineering-grade support for UE5/Cocos
- Auto-scaling loop

---

## 3. First Principles

### 3.1 Runtime First

Prioritize runtime correctness before discussing platform capabilities.

The ordering is as follows:

1. Tick consistency
2. Correct Entity lifecycle
3. Correct message routing
4. Correct property synchronization
5. Correct persistence
6. Usable observability
7. Enhanced ops capabilities

### 3.2 Single source of truth

A given responsibility may have only one main channel:

- Entity runtime messages may only go through the Runtime Transport
- Control plane messages may only go through the Control Plane
- Cross-Realm async messages may only go through the Cross-Realm Bridge

It is forbidden for the same kind of message to go through two links at once.

### 3.3 Implementation before vision

Any capability that would significantly increase the difficulty of state consistency does not enter the MVP by default:

- Coroutine stack migration
- Arbitrary script hot replacement
- Online protocol changes
- Automatic rebalance of dynamic topology
- Unified advanced query semantics across backends

---

## 4. The Three Planes

What most needs unifying in the current documents is the communication boundary. The MVP fixes it to three planes.

### 4.1 Runtime Data Plane

Responsibilities:

- EntityCall
- Base ↔ Cell control messages
- Entity creation/destruction
- Real → Ghost synchronization
- Server-internal forwarding before Witness → Client synchronization
- Runtime data transfer during migration

Requirements:

- Order preservation
- Low latency
- Backpressure
- Entity-routing awareness

Conclusion:

**The Runtime Data Plane does not go through the MessageBus.**

### 4.2 Control Plane

Responsibilities:

- Process bring-up/take-down
- Routing table publication
- Configuration change broadcast
- Deploy, drain, restart commands
- Monitoring and alerting events

Characteristics:

- Not on the critical path within a tick
- Extra hops allowed
- More focused on observability and management

### 4.3 Cross-Realm Async Plane

Responsibilities:

- Cross-server matchmaking
- Cross-server queries
- Cross-server mail/notifications
- Cross-datacenter bridging

Characteristics:

- Not the entity's local authoritative path
- Explicit timeouts, retries, and compensation allowed
- Decoupled from the Runtime lifecycle

---

## 5. Concurrency and Threading Model

The MVP adopts strict threading constraints.

### 5.1 Entity ownership

Each Entity belongs to only one tick thread at any moment.

Rules:

- Only the owning tick thread may read/write Entity memory
- PropertyBlock is not a concurrent container
- Witness, Ghost, Controller, and Timer all follow the thread the Entity is on

### 5.2 Async callback rules

Async tasks may run on background threads, but their results may only return to the tick thread by "posting an event".

Forbidden:

- Modifying an Entity directly on a background thread
- Blocking on `Future.get()` on the tick thread
- Synchronously waiting for external I/O inside a script callback

Allowed:

- Posting `onDbLoaded` after the DB thread completes
- Posting `onHttpResponse` after the HTTP thread completes
- Posting `onCrossRealmResult` after a cross-server request completes

---

## 6. Tick and Synchronization Baseline

The MVP uniformly adopts the following timing model:

1. Network Ingress
2. Timer
3. Entity Logic
4. Script Callback
5. Sync Build
6. Flush

Key constraints:

- Modifying properties within a tick is allowed
- Within a tick, only dirty marking is done; nothing is sent out immediately
- The sync packet is constructed uniformly at the end of the tick
- The flush is done uniformly at the end of the tick

This rule covers:

- Real → Ghost
- Real → Witness
- Witness → Client

There are only two kinds of exceptions:

- Explicit RPC/event messages
- Lifecycle control messages (creation, destruction, migration control)

Property replication is not RPC and should not be sent immediately during script execution.

---

## 7. Reliability Semantics

The MVP does not implement the full model of "four logical levels, four underlying levels" described in the documents.
The MVP commits to only two kinds of semantics:

### 7.1 Ordered Reliable

Used for:

- EntityCall
- Entity creation/destruction
- Base ↔ Cell control messages
- Migration data
- Route updates

Semantics:

- Order preservation
- Reliable delivery
- Backpressure

### 7.2 Unordered Lossy

Used for:

- Volatile updates such as position/direction
- Recomputable AOI auxiliary notifications

Semantics:

- Droppable
- Not retransmitted
- Overwritten by the next frame

If `LOW/HIGH/CRITICAL` is to be extended later, explicit queues, acknowledgement, and replay mechanisms should be implemented in the Runtime first, and only then should the document abstraction be upgraded.

---

## 8. Migration Baseline

The MVP supports only two kinds of migration:

- Cell migration within a single Realm
- Cross-Space teleport

The MVP does not support:

- Cross-datacenter migration
- Migration of suspended script coroutines
- Migration of external I/O context

The state serializable during migration includes only:

- PropertyBlock
- Space / Position / Direction
- Controller state
- Timer state
- Witness base state

Content that does not enter the migration snapshot:

- Python/Lua call stack
- Future continuations in `yield`
- The context of in-flight HTTP/DB requests

Corresponding strategy:

- Freeze the entity inbox before migration
- Mark unfinished async tasks as invalid or wait for the callback and retry
- On callback arrival, validate by Entity epoch whether it is still valid

---

## 9. Hot Update Baseline

The MVP supports only:

- L1 config hot update
- Restricted L2 script implementation replacement

The MVP does not support:

- L3 def hot update
- L4 structural hot update

The constraints on L2 must be very strict:

- Only method implementations may be replaced
- Method signatures are not modified
- The exposed protocol is not modified
- The property layout is not changed
- No reliance on semantic compatibility with old frames on the stack

If a script update affects the entity protocol, property definitions, or serialization layout, it is always escalated to a rolling release rather than a hot update.

---

## 10. Storage Boundary

The MVP should not first pursue "one interface covering all backend capabilities".

It is recommended to split it into a minimal set of interfaces:

```cpp
class IEntityStore {
public:
    virtual Future<EntityData> load(EntityId id, const EntityDef& def) = 0;
    virtual Future<void> save(EntityId id, const EntityData& data, const EntityDef& def) = 0;
    virtual Future<void> remove(EntityId id) = 0;
};

class IEntityQueryStore {
public:
    virtual Future<std::vector<EntityId>> query(const StorageQuery& q) = 0;
};

class ISchemaMigrator {
public:
    virtual Future<MigrationPlan> planMigration(const EntityDef& oldDef,
                                                const EntityDef& newDef) = 0;
    virtual Future<void> executeMigration(const MigrationPlan& plan) = 0;
};
```

MVP default backends:

- Primary storage: MySQL
- Cache: Redis

MVP default mapping strategy:

- Scalar types: native columns
- VECTOR3: expanded columns
- FIXED_DICT: JSON
- ARRAY: JSON

`EXPAND` / `SUBTABLE` are kept as capability designs, but the first version is not required to fully cover all combinations.

---

## 11. Client and Toolchain Scope

The MVP client guarantees only that the full chain works on the Unity side:

- Login
- Reconnection after disconnect
- Entity creation/destruction
- Property synchronization
- Position interpolation
- exposed method invocation

UE5/Cocos may be kept as targets in the documents, but their implementation priority is later than Unity.

The first version of code generation commits only to:

- Entity definition generation
- exposed method stub generation
- Basic serialization code generation

It does not commit to completing in the first version:

- Deep Blueprint integration
- Equivalent support for editors and script debugging tools across all three platforms
- A complex prediction/rollback framework

---

## 12. Observability Baseline

MVP observability requires only three things:

1. Structured logging
2. Basic Metrics
3. Key-path Trace

Required metrics:

- tick duration
- entity count
- queue backlog
- transport backpressure
- db load/save latency
- script error count

MVP optional items:

- Full script breakpoint debugging
- Auto-scaling
- Flame graph export

Observability must serve Runtime debugging, rather than first pursuing platform completeness.

---

## 13. MVP Implementation Order

### Phase A: Minimal modernized runtime loop

- TickScheduler
- Entity / BaseCell model
- PropertyBlock
- Base ↔ Cell EntityCall
- SingleCell Space
- Witness synchronization
- Persistence (FileEntityStore, with MySQL as the Phase B target)
- Minimal LoginApp login access (KBEngine-style, see chapter 12 of `5-access-and-control-plane/01-gateway-and-login`)

### Phase B: Complete online gameplay and basic ops

- AOI cross-linked list
- Controller
- Redis session/rate limiting
- Unity codegen
- Basic Metrics / Logs
- Read-only Ops inspect
- Channel / Bundle state statistics

### Phase C: Bring in BigWorld-level system-area prototypes

- Restricted script hot update
- Migration routing window
- BackupTopologyCoordinator / backup-recovery prototype
- Entity / EntityType load profiler prototype
- Basic Trace

Only after Phase A is stable should the following be advanced:

- Static multi-Cell
- Cross-server bridging
- Auto-scaling
- Multiple storage backends

---

## 14. Relationship to Existing Documents

This document constrains the existing designs as follows:

- In `2-replication-and-space/03-runtime-communication-and-transport`, the boundary of the Runtime Data Plane takes precedence over any MessageBus extension statements
- `2-replication-and-space/04-property-replication` takes the uniform flush at the end of the tick as authoritative
- `2-replication-and-space/05-entity-migration` does not commit to script stack migration
- The multi-capability interfaces of `4-data-and-ops/02-persistence` should be split by implementation stage
- `5-access-and-control-plane/02-message-bus-and-cross-realm` no longer carries the EntityCall main path
- The MVP of `7-scripting-and-client/02-hot-update` supports only L1 + restricted L2
- The MVP of `7-scripting-and-client/03-script-debug` requires only traceback / stack dump / Entity context inspect
- The MVP of `7-scripting-and-client/04-client-sdk` is Unity-first
- The MVP of `5-access-and-control-plane/05-telemetry-and-debug` requires only logs + metrics + key traces

If a subsequent per-topic document conflicts with this page, this page is the current implementation baseline until the corresponding per-topic document is revised.
