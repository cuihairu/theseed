# Server Merge — Server Merge Toolchain

> English | **中文**

> Server merge is a hard requirement of game operations.
> Origin: the data toolchain ideas of BigWorld `consolidate_dbs / transfer_db / sync_db`.
> KBEngine has no complete server merge toolchain, and can only serve as a reference for the gap.
> theseed needs a complete server merge toolchain: ID remapping + conflict resolution + reference repair.
> But this document only discusses "merging business data across servers", and does not cover SecondaryDB transfer and consolidation.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld implements it

```
BigWorld's server merge is not a single script moving databases:
  - transfer_db / consolidate_dbs / sync_db are different tools
  - Before and after a merge, repair and relationship repair are often also needed
```

### How KBEngine implements it

```
KBEngine basically has no complete server merge toolchain.
If a merge is needed, data transfer and repair logic usually have to be added by hand.
```

### Pros and cons

```
BigWorld's pros:
  - Data merge responsibilities are separated
  - Better suited to large-scale operational actions

KBEngine's pros:
  - Simple
  - Suited to small-scale or manual handling

Shared cons:
  - Server merge is always a high-risk operation
```

### theseed's trade-off

```
theseed separates server merge from SecondaryDB transfer,
because they are two different classes of data ops problems,
and must not be written as the same thing.
```

---

## 0. Scope boundary

```
This document is responsible for:
  - Merging business data across Realms / servers
  - EntityID / unique index / reference relationship repair
  - Pre-merge analysis, preview, execution and rollback

This document is not responsible for:
  - BaseApp local SecondaryDB snapshot / transfer
  - generation consolidate
  - The Schema sync and repair platform
```

BigWorld's several kinds of tools should be split apart in design:

| Tool | Goal | Belongs to this document |
|------|------|------|
| merge | Merging business data across servers | Yes |
| transfer_db | SecondaryDB file transfer | No |
| consolidate_dbs | SecondaryDB consolidation | No |
| sync_db | Schema / data sync and repair | No |

For related boundaries see:

[03-local-archive-and-secondary-db](03-local-archive-and-secondary-db.md)

---

## 1. Conflict scenarios

```
Server A and server B merge into server C:

Conflict 1: EntityID overlap
  Server A: Player id=10042 (Alice)
  Server B: Player id=10042 (Bob)

Conflict 2: Unique index conflict
  Server A: Player name="Alice"
  Server B: Player name="Alice" (a different person)

Conflict 3: Broken reference data
  Mail.sender still points to the old ID

Conflict 4: Auto-increment ID overlap

Conflict 5: Invalid EntityCall reference
```

---

## 2. Server merge flow

```
theseed-merge tool:

Phase 1: Analysis
  ├─ Read the def files of both servers (ensure schema compatibility)
  ├─ Scan the databases of both servers
  ├─ Detect conflicts: ID range overlap, unique index conflicts, reference relationship graph
  └─ Generate a conflict report

Phase 2: ID remapping
  ├─ Offset mapping: server B's IDs + offset
  ├─ Namespace mapping: id → realm_a:{id}
  └─ Complete reallocation

Phase 3: Unique index conflict resolution
  ├─ name → automatically add a suffix ("Alice" → "Alice_B")
  ├─ account → merge accounts (manual confirmation)
  └─ Custom fields → script callback

Phase 4: Reference data repair
  ├─ Mail.sender/receiver → new IDs
  ├─ Guild.leader/members → batch replacement
  ├─ EntityCall references → scan BLOB/JSON and replace
  └─ SUBTABLE foreign key update

Phase 5: Data merge
  ├─ Merge in dependency order
  ├─ Validate foreign key integrity
  └─ Rebuild indexes

Phase 6: Validation
  ├─ Data integrity check
  ├─ Unique index validation
  ├─ Foreign key consistency validation
  └─ Generate a merge report
```

---

## 3. Server merge configuration

```xml
<!-- config/merge.xml -->
<merge xmlns="https://theseed.dev/schema/merge">

    <sourceRealms>
        <realm name="realm_a">
            <database host="mysql-a.internal" port="3306" name="theseed_game"/>
        </realm>
        <realm name="realm_b">
            <database host="mysql-b.internal" port="3306" name="theseed_game"/>
        </realm>
    </sourceRealms>

    <targetRealm name="realm_merged">
        <database host="mysql-merged.internal" port="3306" name="theseed_game"/>
    </targetRealm>

    <idRemap strategy="offset">
        <offset realm="realm_a" value="0"/>
        <offset realm="realm_b" value="auto"/>
    </idRemap>

    <conflictResolution default="suffix">
        <field name="playerName" strategy="suffix" suffixTemplate="_{realm}"/>
        <field name="account" strategy="script" script="merge_account_conflict"/>
    </conflictResolution>

    <relationFix autoScanJson="true" autoScanBlob="true">
        <explicitRelation table="tbl_Mail" fields="sender_id,receiver_id"/>
        <explicitRelation table="tbl_Guild" fields="leader_id"/>
    </relationFix>

</merge>
```

---

## 4. CLI tool

```bash
# Analyze conflicts
theseed-merge analyze --config config/merge.xml --output report.json

# Preview (dry run)
theseed-merge preview --config config/merge.xml

# Execute the merge
theseed-merge execute --config config/merge.xml --output merge_result.json

# Rollback
theseed-merge rollback --config config/merge.xml --from merge_result.json
```

---

## 5. Relationship with other data ops capabilities

Server merge is not the entirety of the data ops surface.

theseed needs to explicitly split four lines:

```
1. Primary persistence
   - load / save / remove / query

2. Server merge
   - merge analyze / preview / execute / rollback

3. Local archive staging
   - snapshot / transfer / consolidate / cleanup

4. Sync and repair
   - schema diff / data repair / consistency check
```

If these four classes of capability are mixed into one "all-powerful tool", the boundaries will become very confusing later on.
