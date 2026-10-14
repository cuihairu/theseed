# Fault Tolerance — Three-Level Fault Tolerance

> **English** | [中文](01-fault-tolerance.zh.md)

> Origin: BigWorld `Reviver / BackupSender / Archiver`.
> Reference implementation: KBEngine's basic process recovery and persistent recovery paths.
> This is one of the areas where KBEngine lags furthest behind, but it is also the area where theseed should least over-commit at the `MVP` stage.
> The current implementation baseline follows [0-foundation/01-mvp-architecture-baseline](../0-foundation/01-mvp-architecture-baseline.md).

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
BigWorld's fault tolerance is not a single backup point, but a chain:
  - Reviver is responsible for process startup
  - BackupSender is responsible for the state backup chain
  - Archiver is responsible for persistent archiving
  - Manager is responsible for scheduling and recovery
```

### How KBEngine Implements It

```
KBEngine also has recovery and supervision ideas,
but overall it leans toward basic process recovery and database persistence,
and does not have as complete a high-availability system as BigWorld.
```

### Pros and Cons

```
BigWorld's advantages:
  - Complete disaster recovery chain
  - Systematized recovery boundary

KBEngine's advantages:
  - Simple
  - Easier to get running first

Shared disadvantages:
  - Real HA is highly complex
  - Requires additional definition of backup consistency and recovery ordering
```

### theseed's Trade-offs

```
theseed MVP only commits to basic recovery and supervision,
treating full HA as a separate phase,
to avoid mistakenly writing BigWorld-level disaster recovery into the current implementation boundary.
```

### Why HA Cannot Be Crammed Entirely into This Document

```
Fault Tolerance only answers:
  How the system recovers after a process dies

It does not directly answer:
  - How the hot-standby responsibility chain is organized
  - How the backup target is switched
  - Who has the authority to decide the recovery source

These belong to BackupHash / HA Recovery Chain,
and cannot continue to be mixed into the "recovery overview".
```

---

## 0. MVP Boundary

```
MVP first guarantees:
  - Process supervision and automatic startup
  - Recoverability based on database archiving
  - Basic route repair and client reconnection

MVP does not directly commit to:
  - Complete cross-BaseApp in-memory hot standby
  - A unified recovery target of < 5 seconds
  - Low RPO / low RTO that holds for all entity types
```

```
In other words:
  ProcessSupervisor belongs to MVP
  Archiver belongs to MVP
  EntityBackup is a Phase 2 prototype capability and should not be written as a hard capability that currently exists
```

---

## 1. Three-Level Assurance System

```
Level 1: ProcessSupervisor (MVP)
  Origin: BigWorld Reviver
  Goal: automatic startup after process death, re-registration, restore routing table

Level 2: EntityBackup (Phase 2)
  Origin: BigWorld BackupSender
  Goal: cross-process hot standby for Base entities, reducing recovery time and data loss

Level 3: Archiver (MVP)
  Origin: BigWorld Archiver / KBEngine writeToDB
  Goal: periodic archiving to persistent storage, providing a recoverable baseline
```

---

## 2. MVP Recovery Model

### 2.1 ProcessSupervisor

```
Responsibilities:
  - Detect BaseApp / CellApp / Gateway process death
  - Automatically start new processes by role
  - Have new processes re-register with the Control Plane
  - Trigger routing table re-publication
```

### 2.2 Archiver Recovery

```
Main recovery path under MVP:

Process death
  → Supervisor starts a new process
  → Process rejoins the cluster
  → Base entities are restored from database archive on demand
  → Client re-binds the session after reconnecting
```

```
Recovery guarantees:
  - Can recover to the state of the most recent successful archive
  - Does not commit to recovering all non-archived in-memory state
  - Does not commit to recovering all in-flight runtime messages
```

### 2.3 RPO / RTO Notes

```
Recommended phrasing for MVP:

RPO:
  - Determined by the archive period
  - Usually "at most the loss of non-persisted state within one archive period"

RTO:
  - Jointly determined by process startup, database recovery, and client reconnection
  - Do not hard-code a global second-level metric
```

```
Reason:
  Before there is cross-process hot standby, WAL, and a unified in-flight drain mechanism,
  hard-coding "< 5 second recovery, at most 5 seconds lost" is a vision, not a design constraint.
```

---

## 3. BigWorld's SecondaryDB Is Not Equal to EntityBackup

In BigWorld's implementation, `Archiver` often appears together with `SecondaryDB`.

But note:

```
What SecondaryDB solves:
  - Local archive staging
  - Flushing to disk during shutdown / retirement windows
  - Subsequent unified consolidation

What EntityBackup solves:
  - Cross-BaseApp in-memory hot standby
  - Lower RPO / faster recovery
```

Therefore, they must be separated in the design:

```
Archiver / LocalArchiveStore
  ≠ EntityBackup
```

Conversely:

```
As long as the core goal of a scheme is:
  - Lower RPO / RTO
  - Preserve in-memory state before the primary instance is flushed to the database

then it is discussing hot standby,
not an archive staging layer.
```

The constraint on theseed is:

```
MVP:
  - Only commit to "primary database archive + recoverable"
  - Do not write SecondaryDB / LocalArchiveStore as a capability that currently exists

Phase 2 / 3:
  - If the BigWorld-level data ops surface needs to be filled in, then introduce LocalArchiveStore
  - But this still cannot replace EntityBackup
```

---

## 4. EntityBackup — Phase 2 Capability

```
Problem: When BaseApp crashes, the un-flushed data of all entities on it may be lost.

BigWorld's approach:
  - BackupSender sends Base entity data to another BaseApp
  - The backup is held in memory
  - On crash, recover from the backup BaseApp

theseed's direction:
  - Introduce BackupHash + cross-BaseApp in-memory hot standby
  - But this capability belongs to Phase 2, not an MVP must-have
```

### 4.1 Reserved Interfaces

```cpp
// runtime/EntityBackup.h

class EntityBackupManager {
public:
    void setBackupPolicy(const BackupPolicy& policy);
    void registerBackupTarget(EntityId id, ComponentId backupBaseApp);
    void backupEntity(Entity* entity);
    void restoreFromBackup(ComponentId deadBaseApp,
                           ComponentId newBaseApp);

    bool hasBackup(EntityId id) const;
    ComponentId getBackupLocation(EntityId id) const;
};

struct BackupPolicy {
    Duration backup_interval = 5s;
    int max_backups_per_entity = 3;
    bool compress_backup = true;
    size_t max_backup_memory_mb = 512;
};
```

### 4.2 Phase 2 Recovery Flow

```
BaseApp_A crashes
  → Control Plane starts BaseApp_A'
  → Query BackupHash
  → Pull the Base entity snapshot from the backup BaseApp
  → Restore properties / routing / session
  → Notify the client to reconnect
```

```
Preconditions:
  - The backup replication chain is already stable
  - Base / Cell route switching is observable
  - Session recovery and async callback epoch validation are complete
```

---

## 5. LocalArchiveStore — Phase 2 / 3 Data Ops Enhancement

If theseed is to fill in the BigWorld-level data ops surface in the future, a layer should be added:

```
LocalArchiveStore
  - Local archive staging
  - generation rotate
  - snapshot / transfer / consolidate
```

But the document boundary must be made clear:

```
It is not:
  - The primary database
  - A hot-standby in-memory replica
  - The query platform of Runtime Core
```

Suggested responsibilities:

```
Runtime:
  - Generate archive snapshots
  - append / flush / rotate

Data Ops:
  - consolidate generation
  - transfer / snapshot / cleanup
  - Validation and synchronization
```

Related design:

`../4-data-and-ops/03-local-archive-and-secondary-db.md`

For the hot-standby chain and the recovery authority boundary, see:

`./02-backup-hash-and-ha.md`

---

## 6. Phased Goals

```
Phase A (MVP)
  - Supervisor automatic startup
  - Archiver recoverable
  - Client reconnectable

Phase B
  - Base hot-standby prototype
  - Automated route repair
  - Observable recovery flow

Phase C
  - Stable cross-BaseApp in-memory hot standby
  - Lower RPO / RTO
  - Backup consistency monitoring
```

---

## 7. Comparison with the Two Engines

| Dimension | BigWorld | KBEngine | theseed |
|------|---------|---------|---------|
| Process supervision | Reviver automatic startup | None built in | MVP does Agent + Control Plane first |
| Entity hot standby | BackupSender cross-process | None (only DB archiving) | Phase 2 goal |
| Main recovery path | In-memory hot standby + archiving | Database recovery | MVP does database recovery first |
| Data loss | < 1 backup period | < 1 Archiver period | MVP determined by the archive period |
| Archiving | Archiver + SecondaryDB + consolidate | writeToDB + Archiver | MVP primary database archiving, re-evaluate LocalArchiveStore in Phase 2 |
