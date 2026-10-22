# SecondaryDB & Local Archive Store — Local Archive Staging Layer

> English | **中文**

> Origin: BigWorld `Archiver / SecondaryDB / consolidate_dbs / transfer_db / sync_db`.
> KBEngine has no complete equivalent layer, and can only serve as a simplified reference for writing directly to the primary database.
> This layer is the one most easily misread as the "primary database" or a "hot backup", but what it actually solves is a different class of problem.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld implements it

```
BigWorld's SecondaryDB is more like a local archive staging layer:
  - Receives archive data
  - Provides an intermediate state for consolidate / transfer / sync
  - It is neither the primary persistence database, nor the hot-standby body of BaseApp itself
```

### How KBEngine implements it

```
KBEngine has no complete counterpart.
It is closer to writing data directly to the primary database, lacking an independent local archive staging layer.
```

### Pros and cons

```
BigWorld's pros:
  - Data ops actions can be split apart
  - Convenient for moving, merging and repairing

KBEngine's pros:
  - Simple
  - One fewer storage state layer

Shared cons:
  - The boundary between the staging layer and the primary database must be made explicit, otherwise it very easily becomes confusing
```

### theseed's trade-off

```
theseed first abstracts LocalArchiveStore,
then decides on the concrete backend,
and does not hard-wire implementation details such as SQLite / RocksDB into the architectural premises ahead of time.
```

---

## 0. Design boundary

```
The MVP does not promise:
  - A built-in SecondaryDB
  - A unified implementation of the local archive staging layer
  - Fast-recovery goals based on the local staging layer

If Phase 2 / 3 needs to fill in a BigWorld-level data ops surface:
  - First abstract it as LocalArchiveStore
  - Then decide on the concrete backend
  - Do not hard-wire SQLite / RocksDB directly as an architectural premise
```

```
Three things must be distinguished first:
  - Primary Store: the primary persistence database, the final authority
  - LocalArchiveStore: the local archive staging layer, awaiting consolidation
  - EntityBackup: cross-process hot standby, used for faster recovery
```

---

## 1. What it actually is

BigWorld's `SecondaryDB` is neither the business primary database, nor an in-memory hot standby between BaseApps.

It is closer to:

```
BaseApp local archive staging layer
  - Receives archive snapshots of Base entities
  - Flushes to disk quickly on the local machine
  - Periodically committed / generation-switched by the Archiver
  - Later consolidated back into the primary database uniformly by DBApp
```

Therefore the problems it solves are:

```
1. Archive write amplification
   - Prevents all periodic archives from being pushed directly onto the remote primary database

2. Shutdown / retirement flush window
   - First writes entity snapshots safely to the local machine, then enters unified consolidation

3. Data ops movability
   - Can snapshot / transfer / consolidate / sync

4. Primary database pressure isolation
   - Splits "online archive writes" and "centralized consolidation" into two stages
```

---

## 2. Do not confuse it with the other two layers

| Layer | Role | Typical medium | Recovery semantics |
|------|------|----------|----------|
| Primary Store | Final authoritative persistence | MySQL / PostgreSQL / MongoDB | Loaded on demand after a process restart |
| LocalArchiveStore | Local archive staging and snapshots awaiting consolidation | SQLite / other local embedded storage | Flush to disk first, then consolidate, not directly equal to hot recovery |
| EntityBackup | Cross-process hot standby | Another BaseApp's memory | Reduces RPO / RTO |

Key distinction:

```
LocalArchiveStore is not meant to "replace the primary database".
LocalArchiveStore is also not meant to "replace hot standby".
It merely splits the archive path into:
  runtime snapshot write → local staging → unified consolidation
```

---

## 3. BigWorld-style data flow

If theseed fills in this layer in the future, it is recommended to first understand it in terms of BigWorld's responsibilities, rather than agonizing over the concrete backend first.

```
Base.writeToDB / autoArchive
  → generate entity persistence snapshot
  → append to LocalArchiveStore
  → Archiver flush / commit
  → rotate generation
  → the DB-side Consolidator consolidates uniformly into the Primary Store
  → clean up old generations
```

This chain naturally yields two boundaries:

```
Boundary A: the runtime is only responsible for generating snapshots and appending writes
Boundary B: the data platform is responsible for consolidation, transfer, cleanup and validation
```

---

## 4. Abstraction design

If theseed needs this layer in the future, it should first abstract the "local archive staging responsibility", rather than leaking some database API directly into the kernel.

### 4.1 Local archive staging interface

```cpp
// storage/ILocalArchiveStore.h

struct ArchiveSnapshot {
    EntityId entityId;
    EntityTypeId entityTypeId;
    TickId tickId;
    std::vector<std::byte> payload;
};

struct ArchiveGenerationMeta {
    ArchiveGenerationId id;
    std::string location;
    TickId beginTick;
    TickId endTick;
    size_t snapshotCount;
};

class ILocalArchiveStore {
public:
    virtual ~ILocalArchiveStore() = default;

    virtual Future<void> append(const ArchiveSnapshot& snapshot) = 0;
    virtual Future<void> flush() = 0;
    virtual Future<ArchiveGenerationMeta> rotate() = 0;

    virtual Future<std::vector<ArchiveGenerationMeta>> listGenerations() = 0;
    virtual Future<void> removeGeneration(ArchiveGenerationId id) = 0;
};
```

### 4.2 Consolidation interface

```cpp
// storage/IArchiveConsolidator.h

struct ConsolidationReport {
    size_t mergedSnapshots = 0;
    size_t skippedSnapshots = 0;
    size_t failedSnapshots = 0;
};

class IArchiveConsolidator {
public:
    virtual ~IArchiveConsolidator() = default;

    virtual Future<ConsolidationReport> consolidate(
        const ArchiveGenerationMeta& generation,
        IEntityStore& primaryStore) = 0;
};
```

Design requirements:

```
1. Runtime Core does not depend on consolidate / transfer / sync details
2. ILocalArchiveStore does not carry the responsibility of a complex query platform
3. IArchiveConsolidator does not pollute the Entity load/save main path in reverse
```

---

## 5. Relationship with the primary persistence interface

`IEntityStore` handles primary database reads and writes, and `ILocalArchiveStore` handles local archive staging; the two should not be merged into one "big storage interface".

Bad smell:

```
IStorageBackend:
  - load / save / remove
  - raw SQL
  - merge
  - schema migration
  - archive append
  - archive rotate
  - consolidate
```

This would lead to:

```
1. Runtime Core is forced to know data ops details
2. The data toolchain becomes reversely coupled to the online storage interface
3. The storage abstraction degenerates into a "capability bucket"
```

Therefore the recommended responsibility split is:

```
Online main path
  - IEntityStore

Business query
  - IEntityQueryStore

Local archive staging
  - ILocalArchiveStore

Offline consolidation and ops
  - IArchiveConsolidator
  - IMergeBackend
  - ISchemaMigrator
```

---

## 6. Data ops toolchain boundary

BigWorld's data ops surface is not just "server merge".

If theseed is to cover this layer, it is recommended to split it into four kinds of tools:

| Tool category | Goal | Belongs to this document |
|------|------|------|
| merge | Merging business data across Realms / servers | No |
| snapshot / transfer | Local archive file snapshots and cross-machine transfer | Yes |
| consolidate | Consolidating generations back into the primary database | Yes |
| sync / repair | Schema or data validation, repair, sync | No |

That is to say:

```
Server Merge
  ≠ SecondaryDB transfer
  ≠ consolidation
  ≠ sync / repair
```

For the complete toolchain boundary see:

`./04-data-ops-toolchain.md`

---

## 7. Backend selection principles

The backend selection for this layer must be subordinate to the responsibility, rather than conversely letting some engine define the responsibility.

### 7.1 If the goal is a BigWorld-style local archive staging layer

More emphasis on:

```
  - Single-machine single-writer stability
  - Simple file-level transfer and cleanup
  - Easy generation rotation
  - Crash recovery and ops transparency
```

In this scenario, `SQLite` is a reasonable candidate.

### 7.2 If the goal has already escalated to a "local latest-state store"

More emphasis on:

```
  - High-frequency write throughput
  - Fast key → latest snapshot reads
  - Accumulating local state for longer periods
  - Stronger local recovery capability
```

This is no longer the `SecondaryDB` in BigWorld's original sense, but closer to:

```
LocalStateStore
```

In this scenario, `RocksDB` is worth evaluating separately, but should not be directly equated with `SecondaryDB`.

If we look only at the candidate choices for this layer, the conclusion can be stated more firmly:

| Candidate | Fitness | Main reason |
|------|------|------|
| SQLite | High | Single-machine single-writer; archive generations, transfer and cleanup are all simpler |
| RocksDB | Medium | Better suited to high-frequency key-value state, but already more like LocalStateStore |
| LevelDB | Medium-low | Usable, but its overall ecosystem and long-term maintainability are less clear than either SQLite or RocksDB |
| Developing a custom file format | Low | Easy to end up writing archiving, transfer, cleanup and validation all as fragile scripts |

So the reason the current stage does not outright kill off concrete backends is:

- The core of SecondaryDB is the responsibility, not some engine name
- The MVP only needs to keep the abstraction first
- The actual selection should wait until the goal forks between "archive staging" and "local latest-state store"

Put more directly:

```
If the goal is "archive staging + generation consolidation", SQLite is closer to the responsibility.
If the goal is "local latest state + high-throughput KV", RocksDB is closer to the responsibility.
LevelDB is usable, but does not constitute a preferred recommendation.
```

---

## 8. theseed's recommended wording

It is recommended to unify the documentation around the following set of boundaries:

```
MVP:
  - Primary persistence + the Archiver main path
  - Does not promise LocalArchiveStore

Phase 2:
  - Evaluate introducing LocalArchiveStore
  - Make explicit the snapshot / consolidate / cleanup toolchain

Phase 3:
  - Interlock with high availability, shutdown, and the data ops platform
  - Then consider whether LocalStateStore-level enhancement is needed
```

---

## 9. Comparison with the two engines

| Dimension | BigWorld | KBEngine | theseed |
|------|---------|---------|---------|
| Archive main path | Archiver + SecondaryDB + consolidate | writeToDB + Archiver | MVP archives to the primary database first |
| Local staging layer | Yes | No explicit layer | Evaluated in Phase 2 / 3 |
| Data ops surface | transfer / consolidate / sync fairly complete | Weaker | Needs independent design |
| Hot standby | BackupSender | No built-in | Counted separately, not mixed with SecondaryDB |
