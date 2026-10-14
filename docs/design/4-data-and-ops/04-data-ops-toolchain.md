# Data Ops Toolchain — Snapshot, Transfer, Consolidate, Sync and Repair

> **English** | [中文](04-data-ops-toolchain.zh.md)

> Origin: BigWorld `transfer_db / consolidate_dbs / sync_db / DBApp consolidateData`.
> KBEngine's toolchain is thin, and mainly serves as a reference for basic persistence management.
> This document takes over the "data ops toolchain", not Runtime Core, and not server merge alone.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld implements it

```
BigWorld splits the data ops toolchain very finely:
  - transfer_db
  - consolidate_dbs
  - sync_db
  - repair / consolidateData
```

### How KBEngine implements it

```
KBEngine's toolchain is relatively thin,
and revolves more around basic persistence and simple management.
```

### Pros and cons

```
BigWorld's pros:
  - Each tool's responsibility is clear
  - Ops flows are composable

KBEngine's pros:
  - Lightweight
  - Does not make the toolchain too heavy

Shared cons:
  - The more tools there are, the more easily the boundaries get confused
```

### theseed's trade-off

```
theseed keeps the toolchain layering,
but defines transfer / consolidate / sync / repair separately,
rather than kneading them into one vague "ops tool".
```

---

## 0. Design boundary

```
This document is responsible for:
  - snapshot / transfer / consolidate of local archive generations
  - Data ops flows between the primary database and the staging layer
  - The sync and repair boundary for schema / consistency

This document is not responsible for:
  - The Entity load / save main path
  - Server merge business conflict resolution
  - Base entity hot-standby routing
```

Relationship with other documents:

```
02-persistence
  - Online primary persistence interface

03-server-merge
  - Merging business data across servers

03-local-archive-and-secondary-db
  - What LocalArchiveStore is

This document
  - How ops moves, consolidates and validates this data
```

---

## 1. Why it must be a separate document

BigWorld's data-layer difference is not just "having a SecondaryDB".

The real system surface lies in its accompanying ops chain:

```
1. Online archives first land in local staging
2. The staging can be snapshot / transferred
3. DBApp can consolidate during startup or shutdown phases
4. There is also an out-of-database repair capability such as sync / repair
```

If these capabilities continue to be scattered only across:

```
Persistence
SecondaryDB
Server Merge
```

Two problems arise:

```
1. The boundary between the online main path and the offline toolchain becomes confused
2. "Server merge" is mistaken for the entirety of data ops capability
```

---

## 2. The four lines of the data ops surface

theseed should explicitly split the data ops surface into four lines:

| Line | Goal | Typical trigger |
|------|------|----------|
| snapshot / transfer | Moving staging data or primary database snapshots | Machine migration, backup, shutdown |
| consolidate | Consolidating generations back into the Primary Store | Startup, shutdown, scheduled windows |
| sync | Aligning schema / metadata | Upgrade, environment repair |
| repair | Consistency checks and repair | After a failure, offline inspection |

Do not lump them together under the general name "database tools" any more.

---

## 3. Core object model

### 3.1 Primary Store

```
The final authority
```

### 3.2 Archive Generation

```
A set of local archive snapshots formed by some rotate
```

### 3.3 Snapshot Package

```
A movable, verifiable snapshot package
```

### 3.4 Consolidation Job

```
An offline or semi-online job that consolidates a generation back into the primary database
```

### 3.5 Sync / Repair Job

```
A job targeting schema, indexes, reference relationships and consistency validation
```

---

## 4. Typical data flows

### 4.1 Online archive enhancement path

```
Runtime / Archiver
  → append snapshot
  → LocalArchiveStore.flush
  → rotate generation
  → Data Ops takes over
```

### 4.2 Transfer path

```
generation
  → snapshot package
  → checksum / manifest
  → transfer to remote node or object storage
```

### 4.3 Consolidation path

```
generation
  → validate manifest
  → consolidate into Primary Store
  → mark committed
  → cleanup old generation
```

### 4.4 Repair path

```
schema diff / consistency scan
  → report
  → operator review
  → repair plan
  → apply
```

---

## 5. Snapshot

### 5.1 Responsibility

```
snapshot is responsible for:
  - Exporting a batch of generations or the primary database into a movable package
  - Generating manifest, checksum and version information
  - Providing input for transfer / restore / audit
```

### 5.2 What it must not be written as

```
snapshot is not:
  - A runtime real-time query replica
  - A hot-standby recovery chain
  - A server-merge import format
```

### 5.3 Design requirements

```
1. A snapshot package must be verifiable
2. A snapshot must explicitly record the schema version
3. A snapshot must not implicitly write back to the primary database
```

---

## 6. Transfer

BigWorld's `transfer_db` shows that transfer is itself a separate tool surface.

### 6.1 Responsibility

```
transfer is responsible for:
  - Moving a snapshot / generation to the target node
  - Controlling bandwidth, retries and resumable retransmission
  - Guaranteeing that the target side can verify integrity
```

### 6.2 Design requirements

```
1. transfer does not directly modify the Primary Store
2. transfer must have manifest validation
3. A successful transfer does not equal a successful consolidation
```

---

## 7. Consolidate

BigWorld's `DBApp::consolidateData()` shows that consolidation is an independent stage, not an incidental action of archive append.

### 7.1 Responsibility

```
consolidate is responsible for:
  - Reading one or more generations
  - Deduplicating, ordering and merging into the Primary Store
  - Marking success / failure / retryable states
```

### 7.2 Semantic requirements

```
1. consolidate is the "primary database write stage"
2. consolidate must be retryable
3. A failed consolidate must not delete the generation by default
4. consolidate needs operator-visible state
```

### 7.3 Trigger timing

```
Startup phase:
  - Handle leftover generations

Shutdown phase:
  - Try to complete the last round of consolidation

Ops window:
  - Periodic consolidation and cleanup
```

---

## 8. Sync

`sync` should no longer be mixed into `merge` or `migration`.

### 8.1 Responsibility

```
sync is responsible for:
  - schema / index / metadata alignment
  - Configuration-based sync between environments
  - The data plane only performs corrections at the "format / metadata consistency" level
```

### 8.2 Typical scenarios

```
1. Schema drift after an upgrade
2. Some environment missing indexes or constraints
3. A generation manifest inconsistent with the primary database metadata
```

---

## 9. Repair

repair's positioning must be more conservative than sync.

### 9.1 Responsibility

```
repair is responsible for:
  - Discovering and repairing corrupt data
  - Handling orphan references / broken unique indexes / partial-write residue
  - Outputting repair reports and audit records
```

### 9.2 Design constraints

```
1. repair reports by default first, then applies
2. repair must support dry-run
3. repair is not allowed to disguise itself as an online main-path capability
```

---

## 10. theseed's toolchain model

### 10.1 CLI proposal

```bash
# snapshot
theseed-data snapshot create --source local-archive --generation latest --output snapshot.tar.zst
theseed-data snapshot verify --input snapshot.tar.zst

# transfer
theseed-data transfer push --input snapshot.tar.zst --target 10.0.0.12:/data/inbox
theseed-data transfer pull --source 10.0.0.12:/data/inbox/snapshot.tar.zst

# consolidate
theseed-data consolidate run --generation 2026-05-21T020000Z
theseed-data consolidate status --job 9f4d8e

# sync / repair
theseed-data sync plan --schema defs/current
theseed-data repair scan --check references,unique,json
theseed-data repair apply --plan repair-plan.json
```

### 10.2 Responsibility layering

```
Runtime
  - Only responsible for generating archive snapshots

Data Ops Agent / CLI
  - snapshot / transfer / consolidate / sync / repair

Ops Control Plane
  - Only responsible for triggering, viewing state and auditing
```

---

## 11. Core interface proposal

### 11.1 Snapshot

```cpp
// dataops/IArchiveSnapshotService.h

struct SnapshotManifest {
    std::string snapshotId;
    std::string schemaVersion;
    std::vector<ArchiveGenerationId> generations;
    uint64 createdAtUnixMs = 0;
};

class IArchiveSnapshotService {
public:
    virtual ~IArchiveSnapshotService() = default;

    virtual Future<SnapshotManifest> createFromGeneration(
        ArchiveGenerationId id,
        const std::string& outputPath) = 0;

    virtual Future<void> verify(const std::string& inputPath) = 0;
};
```

### 11.2 Transfer

```cpp
// dataops/IArchiveTransferService.h

class IArchiveTransferService {
public:
    virtual ~IArchiveTransferService() = default;

    virtual Future<void> push(const std::string& packagePath,
                              const std::string& targetUri) = 0;
    virtual Future<void> pull(const std::string& sourceUri,
                              const std::string& outputDir) = 0;
};
```

### 11.3 Consolidate

```cpp
// dataops/IConsolidationService.h

enum class ConsolidationState {
    Pending,
    Running,
    Succeeded,
    Failed
};

struct ConsolidationJobStatus {
    std::string jobId;
    ArchiveGenerationId generationId;
    ConsolidationState state = ConsolidationState::Pending;
};

class IConsolidationService {
public:
    virtual ~IConsolidationService() = default;

    virtual Future<ConsolidationJobStatus> run(
        ArchiveGenerationId generationId) = 0;
    virtual Future<ConsolidationJobStatus> status(
        const std::string& jobId) const = 0;
};
```

### 11.4 Sync / Repair

```cpp
// dataops/IDataRepairService.h

struct RepairPlan {
    std::string planId;
    std::vector<std::string> checks;
    bool dryRun = true;
};

class IDataRepairService {
public:
    virtual ~IDataRepairService() = default;

    virtual Future<RepairPlan> scan(const std::vector<std::string>& checks) = 0;
    virtual Future<void> apply(const RepairPlan& plan) = 0;
};
```

---

## 12. Relationship with server merge

It must be continuously emphasized:

```
merge
  is the integration of business data across servers

snapshot / transfer / consolidate / sync / repair
  are the data ops capabilities of a single server or a single cluster
```

Both belong to the data toolchain, but they are not one line.

---

## 13. Phased boundary

```
MVP:
  - First keep only primary persistence and a basic backup / restore scheme
  - Does not promise a complete Data Ops Toolchain

Phase 2:
  - LocalArchiveStore + snapshot / consolidate
  - Basic transfer capability
  - Minimal schema alignment for sync

Phase 3:
  - repair / consistency platform
  - Interlock with shutdown, machine migration and cross-environment ops flows
  - Richer object storage and remote transfer backends
```

---

## 14. Comparison with BigWorld / KBEngine / theseed

| Dimension | BigWorld | KBEngine | theseed |
|------|---------|---------|---------|
| SecondaryDB transfer | Has `transfer_db` | Basically none | Phase 2 goal |
| consolidation | Has | Weaker | Phase 2 goal |
| sync / repair | Has a toolchain prototype | Weaker | Needs separate design |
| Layering of merge and data ops | Factually present but documentation not unified | Weak | Explicitly split |
