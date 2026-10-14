# Property Sync — Property Replication and Synchronization

> **English** | [中文](04-property-replication.zh.md)

> Property synchronization is theseed's most core data flow: how entity property changes propagate efficiently to all relevant parties.
>
> Origin: BigWorld `DataLoDLevels / VolatileInfo / EntityCache / Witness`.
> Reference implementation: KBEngine `onDefDataChanged + Witness::update`.
> The current implementation baseline is defined by [0-foundation/01-mvp-architecture-baseline](../0-foundation/01-mvp-architecture-baseline.md).

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
BigWorld's property replication is not simple field synchronization:
  - DataLoDLevels performs leveling
  - VolatileInfo records transient changes
  - EntityCache records the detail level, event number, volatile number, and priority of each observation relationship
  - Witness is responsible for packing and sending dirty data by view within the bandwidth budget
```

### How KBEngine Implements It

```
KBEngine leans more toward event-driven:
  - onDefDataChanged marks changes
  - Witness::update sends uniformly within the tick
  - The replication chain is shorter, but there is also less systematic layering
```

### Pros and Cons

```
Common advantages:
  - Can compress high-frequency property changes into batch processing
  - Has control over view and replication granularity

Common disadvantages:
  - Design complexity is higher than "change and send"
  - Must be strongly coupled with AOI / Witness / migration
```

### theseed's Trade-offs

```
theseed chooses dirty + tick-batch + detail level as the MVP,
while taking BigWorld's EntityCache / priority / bandwidth budget as the later upper bound.
This way it can first land the lightweight main path in the KBEngine reference implementation,
without losing BigWorld's key design for same-screen synchronization in large MMOs.
```

---

## 0. MVP Synchronization General Principles

```
Unified rules under the MVP:

  Property change
    → only mark dirty
    → do not send out immediately during script execution
    → construct the synchronization Bundle uniformly at the end of the tick
    → flush uniformly at the end of the tick

The only exceptions are:
  - Explicit RPC / event messages
  - Lifecycle control messages (create / destroy / migration control)
```

---

## 1. Synchronization Directions

```
Property synchronization has four paths:

Path 1: Real → Ghost (in-CellApp synchronization)
  Trigger: property change onDefDataChanged
  Condition: isReal() && hasGhost() && property marked CELL_PUBLIC
  Method: mark ghost dirty, construct the state-delta Bundle uniformly at the end of the tick

Path 2: Real → Witness (server to client)
  Trigger: property change onDefDataChanged
  Condition: property marked OTHER_CLIENTS or OWN_CLIENT
  Method:
    OWN_CLIENT: mark own-client dirty
    OTHER_CLIENTS: mark witness dirty
    Note: under the MVP, do not send client messages directly in the property setter

Path 3: Witness → Client (batch at the end of the tick)
  Trigger: Witness::tick()
  Content:
    - View enter/leave events
    - Position/orientation volatile updates
    - detailLevel changes
    - Dirty properties filtered by detailLevel and then sent
  Method: one Bundle contains all updates, sent in a single send

Path 4: Entity → Database (persistence)
  Trigger: setDirty() marking + periodic save
  Condition: property marked Persistent
  Method: periodically (e.g. 30s) serialize all dirty properties and write them to the database
```

---

## 2. Dirty Marking System

```cpp
// runtime/DirtyMask.h

// 使用位图标记哪些属性变更了
// uint64 可覆盖 64 个属性，超过则用 vector<uint64>

class DirtyMask {
public:
    void mark(PropertyId id) {
        mask_ |= (1ULL << id);
    }

    bool isDirty(PropertyId id) const {
        return (mask_ & (1ULL << id)) != 0;
    }

    bool any() const { return mask_ != 0; }

    void clear() { mask_ = 0; }

    template<typename Func>
    void foreachDirty(Func&& fn) const {
        uint64_t m = mask_;
        PropertyId id = 0;
        while (m) {
            if (m & 1) fn(id);
            m >>= 1;
            id++;
        }
    }

private:
    uint64_t mask_ = 0;
};
```

```
Notes:
  - PropertyBlock's dirty only means "changed within this tick"
  - It is not equivalent to "already sent to all observers"
  - Different targets (ghost / witness / db) can each have their own send cursor or filtering logic
```

---

## 3. Sync Build & Flush

```
The logic for uniformly constructing the synchronization Bundle at the end of the tick:

1. Collect the lifecycle events of this tick
   - create / leave / destroy
   - detailLevel changes

2. Construct the Ghost state-delta Bundle
   - Only serialize ghost-visible properties
   - Output by entity and property dirty bitmap

3. Construct the Witness / Client Bundle
   - First output view enter/leave
   - Then output property deltas
   - Finally attach volatile data

4. Unified flush
   - Runtime Data Plane: real → ghost
   - Client Channel: witness → client

Notes:
  The responsibility of onDefDataChanged is to "mark dirty", not to "send packets immediately".

Bundle layout:
┌──────────────────────────────────────────────────────────────────────┐
│ Header: message ID (aliasID or utype)                                │
├──────────────────────────────────────────────────────────────────────┤
│ Entity 1:                                                            │
│   entityID (aliasID if available)                                    │
│   detailLevel change flag                                            │
│   ┌─ dirty property bitmap (only properties of current detailLevel)  │
│   │  property value serialization...                                  │
│   └─                                                                 │
│   volatile data (position/orientation)                               │
├──────────────────────────────────────────────────────────────────────┤
│ Entity 2: ...                                                        │
├──────────────────────────────────────────────────────────────────────┤
│ Tail: end marker                                                     │
└──────────────────────────────────────────────────────────────────────┘
```

Bandwidth optimization techniques (from KBEngine):

1. aliasID: when the property count < 255, use 1 byte instead of the 2-byte utype
2. detailLevel: distant entities synchronize only part of their properties
3. Dirty property bitmap: send only the properties that changed
4. volatile threshold: synchronize position/orientation only when the change exceeds the threshold
5. entity alias: send the entity type string the first time, then use a 1-byte alias
