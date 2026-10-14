# Backup Hash & HA Recovery Chain — Hot-Standby Chain, Recovery Authority, and Switch Boundary

> **English** | [中文](02-backup-hash-and-ha.zh.md)

> Origin: BigWorld `BackupHash / BackupHashChain / BackupSender / BaseAppMgr::useNewBackupHash / requestBackupHashChain`.
> KBEngine has no complete equivalent hot-standby responsibility chain and can only serve as a basic recovery reference.
> This document is not repeating `EntityBackup`; it establishes BigWorld's genuinely systematized hot-standby chain as its own topic.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
The core HA difference of BigWorld lies in BackupHash / BackupHashChain:
  - Clear primary / backup ownership
  - The backup chain can be switched
  - On crash / retire there is recovery authority and chain adjustment logic
```

### How KBEngine Implements It

```
KBEngine has no complete equivalent BackupHash system.
It leans more toward basic recovery and persistence, and does not have a BigWorld-level hot-standby chain.
```

### Pros and Cons

```
BigWorld's advantages:
  - Clear hot-standby relationship
  - Systematized recovery chain

KBEngine's advantages:
  - Simple
  - No need to bear complex HA state synchronization

Shared disadvantages:
  - Once hot standby is implemented, recovery authority, switch ordering, and consistency all become high-risk points
```

### theseed's Trade-offs

```
theseed does not write BackupHash into the MVP,
but it must establish this BigWorld-level HA boundary on its own,
to avoid continuing to mistakenly write "high availability" as "process startup + database read recovery" in the future.
```

### Why It Cannot Be Merged Back into Fault Tolerance

```
Fault Tolerance discusses "whether the system can come back after a crash".
BackupHash / HA Chain discusses:
  - Where backup responsibility falls
  - How switching does priming / ack / promote
  - Who arbitrates recovery authority

The two are related, but they are not the same layer of problem.
```

---

## 0. Design Boundary

```
This document covers:
  - The responsibility boundary of BackupHash / BackupHashChain
  - primary / backup ownership
  - The newBackupHash switch flow
  - Recovery authority and chain adjustment on retire / crash

This document does not cover:
  - The primary database archive interface
  - The file organization of LocalArchiveStore
  - Concrete serialization formats and compression algorithms
```

Its relationship to other documents:

```
01-fault-tolerance
  - Explains why multi-level fault tolerance is needed

03-local-archive-and-secondary-db
  - Explains what the local archive staging layer is

This document
  - Explains how a BigWorld-level hot-standby chain actually holds together
```

---

## 1. Why It Must Be a Separate Document

BigWorld's high-availability difference is not about "one more backup replica".

The real difference is that it turns the following into one system:

```
1. Every Base entity can be mapped to a backup destination
2. A backup destination change is not an instantaneous replacement, but has a switch period
3. During retire, the backup hash must be frozen
4. After a BaseApp dies, who recovers and to where are both derivable within the chain
5. BaseAppMgr can distribute the backup hash chain as a cluster-level authority
```

If this layer is not written out, `EntityBackup` will be misunderstood as:

```
"Just throw the entity snapshot to another BaseApp"
```

This is not on the same level as BigWorld's implementation.

More directly:

```
"Having a backup replica"
  is not equal to
"Having a switchable, arbitrable, rebuildable hot-standby chain"
```

---

## 2. What It Actually Is

### 2.1 BackupHash

What `BackupHash` solves:

```
EntityId
  → backup BaseApp location
```

In other words, it describes "where the hot-standby replica of a given Base entity should land".

### 2.2 BackupHashChain

What `BackupHashChain` solves:

```
When a BaseApp dies:
  - How the affected backup responsibilities are re-arranged
  - Which BaseApps are holding backups for which BaseApps
  - Which chain the recovered new instance should rejoin
```

So it is not a simple hash table, but:

```
Cluster-level backup responsibility chain
```

### 2.3 newBackupHash

In BigWorld, the existence of `newBackupHash` shows one key fact:

```
A backup destination change is not an atomic replacement.
The new chain must first be primed, then acked for the switch.
```

This is also a point that theseed documents had not yet explained separately.

---

## 3. Core Concepts

### 3.1 Primary Ownership

```
Primary
  - The current runtime authority of the entity
  - Responsible for serving the outside
  - Responsible for generating backup snapshots
```

### 3.2 Backup Ownership

```
Backup
  - Holds the hot-standby replica
  - Does not become the primary authority of the entity externally
  - Participates in recovery only after the primary instance fails
```

### 3.3 Restore Authority

```
Restore Authority
  - Who has the right to initiate recovery after a crash
  - Which new primary the entity should bind to after recovery
  - Who arbitrates whether the backup snapshot is usable
```

In theseed, this authority should not be scattered across local judgments of each BaseApp, but should be uniformly provided by:

```
Cluster Control Plane / HA Coordinator
```

and issued uniformly.

---

## 4. BigWorld-Style Chain Model

theseed is recommended to state the following model explicitly at the document level:

```
Entity
  → primary BaseApp
  → backup BaseApp (via BackupHash)
  → cluster-level adjustment (via BackupHashChain)
```

At runtime there are three hash views:

```
1. active hash
   - The backup destination currently in effect

2. staging hash
   - The new backup destination being primed

3. chain authority
   - The cluster-level backup responsibility chain authority
```

The corresponding BigWorld semantics can be abstracted as:

```
backupHash
newBackupHash
BackupHashChain
```

---

## 5. Switching Is Not an Instantaneous Operation

BigWorld's `useNewBackupHash` / `ackNewBackupHash` show that switching contains at least two stages:

### 5.1 Priming

```
The old chain is still in effect
The new chain begins receiving initialization backups
```

Purpose:

```
To avoid the new backup side not yet having a complete entity snapshot right after a direct switch
```

### 5.2 Ack & Promote

```
When the new chain completes priming:
  - BaseApp acks the new hash
  - BaseAppMgr / HA Coordinator promotes the new hash to active
  - The old hash is retired
```

This means theseed should not write switching as:

```
replacing the backup target directly after rebalance
```

The correct phrasing should be:

```
prepare
  → prime
  → ack
  → promote
```

---

## 6. Key Constraints During retire

BigWorld has a very strong constraint during retire:

```
A BaseApp in retire must keep its backup hash stable
```

The reason is not an implementation detail, but system correctness:

```
If the backup hash keeps changing during offloading,
then once some entity crashes within the migration window,
the system may be unable to determine where it should recover from, and where it should migrate to after recovery.
```

Therefore theseed should make clear:

```
In the Draining / Retiring state:
  - The primary route may migrate
  - The backup hash should not be rewritten arbitrarily
  - If a switch is needed, it must wait for the current round of offload to complete or for the chain to be rebuilt
```

---

## 7. Recovery Authority on crash

### 7.1 Recovery Flow

theseed is recommended to write the main hot-standby recovery path as:

```
BaseApp_A crash
  → Supervisor / Control Plane detects the failure
  → HA Coordinator queries the chain authority
  → Confirms the backup owners corresponding to the entities on A
  → Starts BaseApp_A' or selects a new primary
  → Restores entity snapshots from the backup owners
  → Repairs routing / session / epoch
  → Publishes the new backup chain
```

### 7.2 Authority Requirements

Three points must be emphasized here:

```
1. The recovery entry point must be arbitrated at a single point
   - To prevent multiple backups from spontaneously "taking over" at the same time

2. The recovery target must be explicit
   - Whether to recover to a new instance in place, or to another capable instance

3. The backup relationship after recovery must be rebuilt
   - Successful recovery does not mean the HA feedback loop is complete
```

---

## 8. Relationship with EntityBackup / LocalArchiveStore

The three must be strictly distinguished:

| Layer | Role | Typical Medium | Semantics |
|------|------|----------|------|
| EntityBackup | Hot-standby snapshot of a single entity | Memory of another BaseApp | Lower RPO / faster recovery |
| BackupHash / Chain | Backup responsibility routing and recovery authority | Cluster control plane state | Decides who the backup should be sent to and from whom to recover after death |
| LocalArchiveStore | Local archive staging | SQLite / other embedded storage | Shutdown flushing, consolidation, transport |

So the relationship is:

```
EntityBackup
  depends on BackupHash / Chain to form systematized HA

LocalArchiveStore
  does not provide backup routing authority
```

---

## 9. theseed's Abstraction Proposal

### 9.1 HA Coordination Interface

```cpp
// ha/IBackupTopologyCoordinator.h

struct BackupRoute {
    EntityId entityId;
    ProcessId primary;
    ProcessId backup;
    uint64 epoch = 0;
};

struct BackupTopologyVersion {
    uint64 version = 0;
    uint64 epoch = 0;
};

class IBackupTopologyCoordinator {
public:
    virtual ~IBackupTopologyCoordinator() = default;

    virtual BackupRoute routeFor(EntityId entityId) const = 0;
    virtual BackupTopologyVersion version() const = 0;

    virtual Future<void> beginRebuild(ProcessId reasonProcess) = 0;
    virtual Future<void> ackPrimed(
        ProcessId processId,
        BackupTopologyVersion version) = 0;
};
```

### 9.2 Hot-Standby Management Interface

```cpp
// ha/IEntityBackupReplicator.h

struct BackupSnapshot {
    EntityId entityId;
    EntityTypeId entityTypeId;
    TickId tickId;
    uint64 topologyEpoch = 0;
    std::vector<std::byte> payload;
};

class IEntityBackupReplicator {
public:
    virtual ~IEntityBackupReplicator() = default;

    virtual Future<void> replicate(const BackupSnapshot& snapshot) = 0;
    virtual Future<void> prime(BackupTopologyVersion version) = 0;
    virtual Future<void> restorePrimary(
        ProcessId deadPrimary,
        ProcessId newPrimary) = 0;
};
```

Design requirements:

```
1. route authority and snapshot transport are layered
2. epoch/version must be explicitly visible
3. restorePrimary is not equal to "just pick a backup and read it out"
```

---

## 10. State Machine Proposal

### 10.1 Topology State

```
Stable
  → Rebuilding
  → Priming
  → Promoting
  → Stable
```

Exception branches:

```
Priming → Aborted
Promoting → Aborted
```

### 10.2 Process-Side State

```
Running
  → Draining
  → Retiring
  → Stopped

Running
  → Failed
  → Recovering
  → Running
```

Requirements:

```
1. While Retiring, an uncontrolled backup topology rebuild must not be accepted
2. After Recovering completes, it must rejoin the backup chain
3. The topology version must be recorded together with the recovery event
```

---

## 11. Phased Boundaries

```
MVP:
  - Does not commit to BackupHash / BackupHashChain
  - Only retains abstract terms and responsibility boundaries
  - Recovery still relies mainly on database archiving

Phase 2:
  - Introduce BackupTopologyCoordinator
  - Support single-replica Base hot standby
  - Support the prepare → prime → ack → promote switch chain

Phase 3:
  - More stable recovery authority arbitration
  - Finer-grained topology health / skew checks
  - Interlock with the rolling update / retire / data ops platform
```

---

## 12. Comparison with BigWorld / KBEngine / theseed

| Dimension | BigWorld | KBEngine | theseed |
|------|---------|---------|---------|
| BackupHash | Yes | No | Phase 2 goal |
| BackupHashChain | Yes | No | Phase 2 goal |
| newBackupHash switch period | Yes | No | Explicitly modeled |
| Freeze backup chain during retire | Yes | No | Explicitly written into the boundary |
| Hot-standby recovery authority | Cluster-level coordination | Basically none | Delegated to HA Coordinator |
