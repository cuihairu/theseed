# BSP Rebalance & Offload — grow/shrink, chunk-aware balance, and retire-offload

> **English** | [中文](07-bsp-rebalance-and-offload.zh.md)

> Origin: BigWorld `InternalNode::doBalance / balanceChildren / balanceOnUnloadedChunks / chunkLimitInDirection / CellData::balance / retireCell / shouldOffload`.
> KBEngine has only thin reference value on dynamic BSP, offload, and chunk-aware balance.
> What this document adds is the genuinely complex layer of BigWorld's dynamic multi-Cell, not another repetition of "Phase 3 supports BSP".

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
BigWorld's BSP is not single-point partitioning, but a complete dynamic scheduling system:
  - grow / shrink
  - chunk-aware balance
  - retire / offload
  - Linked with load bounds and profiler feedback
```

### How KBEngine Implements It

```
KBEngine does not have as complete a dynamic BSP system as BigWorld.
It leans more toward:
  - A fixed spatial model
  - A small amount of manual scheduling
  - First getting migration and AOI running smoothly
```

### Pros and Cons

```
BigWorld's advantages:
  - Suitable for very large worlds and complex load management
  - Can perform automated spatial scheduling

KBEngine's advantages:
  - Light
  - Few boundaries
  - More suitable as a lightweight runtime reference

Common disadvantages:
  - The implementation and verification costs of dynamic rebalance are both high
```

### theseed's Trade-offs

```
theseed keeps BSP only as a Phase 3 placeholder,
does not currently commit to automatic rebalance or offload,
avoiding writing long-term capabilities as an already deliverable implementation surface.
```

---

## 0. Design Boundary

```
This document is responsible for:
  - How BSP partition points grow / shrink
  - How to combine entity load with chunk boundaries during rebalance
  - The relationship between offload / retireCell and rebalance
  - The difference between updateRanges and loadBalance

This document is not responsible for:
  - The cross-linked list AOI
  - witness priority scheduling
  - The migration snapshot serialization protocol
```

Relationship with other documents:

```
03-aoi-and-space
  - Discusses the overall topology framework

05-entity-migration
  - Discusses how entities migrate

11-aoi-update-schemes-and-load-bounds
  - Discusses the load bounds / range update inputs

This document
  - Discusses how BSP moves boundaries and triggers offload based on these
```

---

## 1. Why This Document Must Stand on Its Own

The complexity of BigWorld's `BSP` is not in the four words "tree partitioning".

The real complexity lies in its simultaneously handling:

```
1. Entity CPU / logic load
2. chunk load boundaries
3. ghost distance adjacency cost
4. Protection when a new Cell is not yet ready
5. retire / offload / underload shrink
```

If all of these continue to be summarized only as:

```
Phase 3: BSP / auto rebalance
```

The documentation still does not cover BigWorld's most critical system area.

---

## 2. What BigWorld Is Actually Balancing

From `InternalNode::doBalance`, it can be extracted that it is not single-objective optimization, but rather looks at four kinds of signals at the same time:

```
1. The average load difference between the left and right subtrees
2. Whether maxLoad touches the safety bound
3. Whether there are still unloaded chunks
4. Whether the child Cell has finished being created
```

So `rebalance` is not:

```
Push the boundary toward wherever there are more entities
```

But is closer to:

```
Under the premise of movability,
take the nearest feasible point among the entity boundary, chunk boundary, ghost adjacency cost, and loadSafetyBound
```

---

## 3. The Real Semantics of grow / shrink

### 3.1 Grow

```
One side's boundary expands outward,
meaning that side will take on more spatial area and potential entities
```

### 3.2 Shrink

```
One side's boundary contracts inward,
meaning that side's area is reduced, and it may trigger entity migration out or Cell retirement
```

### 3.3 Asymmetric Constraints

BigWorld's `balanceChildren()` already reflects an important point:

```
One side grows
while the other side shrinks
and the shrinking side is explicitly marked
```

This means theseed's BSP rebalance should not just "update a split value", but must carry:

```
which side grows
which side shrinks
whether shrink may force offload
```

---

## 4. Preconditions Matter More Than the Algorithm Itself

BigWorld has several very critical protection conditions before rebalance:

### 4.1 The New Cell Must Already Be Created

```
childrenCreated = left.hasBeenCreated && right.hasBeenCreated
```

Design implication:

```
The boundary cannot be pushed over before the target Cell is ready,
otherwise other Cells may start offloading first, causing the target to not exist.
```

### 4.2 The retire State Changes the Objective Function

If one side is retiring:

```
Do not continue doing smooth balancing,
but instead prioritize pushing the area toward the non-retiring side.
```

### 4.3 loadSafetyBound Is a Hard Protection

BigWorld explicitly prevents:

```
For the sake of balancing chunk or split,
continuing to grow the side that is already approaching overload.
```

This is also a boundary that the previous theseed documentation had not yet written out separately.

---

## 5. Not Only Entities, But Also Chunks

`balanceOnUnloadedChunks()` and `chunkLimitInDirection()` show that BigWorld's balancing is not purely logical load.

It is also optimizing:

```
Which regions have not yet finished loading the chunks they need
```

This means BSP rebalance has at least two objective functions:

| Mode | Primary Goal |
|------|----------|
| CPU / entity balance | Balance the average logical load |
| unloading / loading aware balance | Reduce incomplete regions caused by unloaded chunks |

This is also why `loadBalance` and `updateRanges` must be kept separate.

---

## 6. entity limit and chunk limit Are Two Different Boundaries

BigWorld's `closestLimit(entityLimit, chunkLimit, direction)` is very critical.

It shows that when a partition point moves, it is subject to at least two constraints:

### 6.1 entityLimit

```
The farthest movable point derived from the entity distribution and loadDiff
```

### 6.2 chunkLimit

```
The farthest movable point derived from chunkBounds + ghostDistance
```

Design implication:

```
The boundary cannot be pushed as far as one wishes,
but can only be pushed to the nearest point jointly allowed by the "load constraint" and the "world asset constraint".
```

This point is one of the most essential differences between BigWorld's multi-Cell world and KBEngine's single Cell world.

---

## 7. ghostDistance Is Not Just an AOI Parameter

In `chunkLimitInDirection()`, BigWorld directly incorporates `ghostDistance` into the partition boundary constraint.

This shows that:

```
ghostDistance
  is not just a synchronization radius parameter
  but also a topology constraint parameter
```

Because after the boundary changes, adjacent Cells still need to maintain correct loading and query capability within the ghost adjacency area.

theseed should write this point out separately, to avoid leaving `ghostDistance` only in the synchronization configuration in the future.

---

## 8. updateRanges and loadBalance Are Not the Same Thing

BigWorld splits the two into two paths:

### 8.1 updateRanges

```
Recompute the spatial ranges of the current BSP nodes and leaves
without needing to make a full load decision
```

### 8.2 loadBalance

```
Decide whether the boundary moves based on the current load and chunk state
triggering grow / shrink / offload / delete cell when necessary
```

theseed has already brought up `load bounds / range update` before; now one more thing must be added:

```
range update is geometric propagation
rebalance is a scheduling decision
```

---

## 9. The Relationship Between offload and retireCell

BigWorld has at least three similar but different actions here:

### 9.1 shouldOffload

```
Allow / forbid a Cell to continue actively migrating entities out
```

### 9.2 offload

```
Transfer some entities or regional work to the other side
```

### 9.3 retireCell

```
Put a Cell onto the retirement path, eventually exiting the partition tree
```

Their relationship is not:

```
rebalance = retire
```

But rather:

```
rebalance
  may only move the boundary
  may trigger offload
  only when shrunk to no area and no entities may it enter delete / retire
```

---

## 10. theseed's Recommended Modeling

### 10.1 BSP rebalance Input

```cpp
// topology/BspRebalanceInput.h

struct BspRebalanceInput {
    SpaceId spaceId;
    Rect2f fullRange;
    float loadSafetyBound = 0.0f;
    bool shouldLimitToChunks = true;
    bool shouldBalanceUnloadedChunks = false;
    float ghostDistanceMeters = 0.0f;
};
```

### 10.2 Node Decision Result

```cpp
// topology/BspRebalanceDecision.h

enum class BspBalanceDirection {
    None,
    Left,
    Right
};

struct BspRebalanceDecision {
    BspBalanceDirection direction = BspBalanceDirection::None;
    float oldSplit = 0.0f;
    float newSplit = 0.0f;
    bool causedShrink = false;
    bool chunkConstrained = false;
    bool loadConstrained = false;
};
```

### 10.3 Control Interface

```cpp
// topology/IBspRebalanceController.h

class IBspRebalanceController {
public:
    virtual ~IBspRebalanceController() = default;

    virtual BspRebalanceDecision evaluate(
        const BspRebalanceInput& input) = 0;

    virtual void apply(const BspRebalanceDecision& decision) = 0;
    virtual void updateRanges(SpaceId spaceId) = 0;
};
```

Design requirements:

```
1. evaluate and apply are separated
2. The result must explicitly state whether it is constrained by chunk or by load
3. Whether shrink will trigger offload / retire must be visible
```

---

## 11. Interface with the Migration Document

This document only defines:

```
When migration is needed
Which side grows/shrinks
Which Cells enter offload / retire
```

The actual migration execution still belongs to [05-entity-migration](05-entity-migration.md).

Therefore the relationship between the two should be written as:

```
BSP rebalance
  → produces migration / offload intent

Entity migration
  → executes data-plane movement safely
```

---

## 12. Phased Boundary

```
MVP:
  - Do not commit to BSP rebalance
  - Only support single_cell

Phase 2:
  - static partition
  - No dynamic grow / shrink
  - Allow manual control of partitioning and offload strategies

Phase 3:
  - Dynamic split position adjustment
  - load-aware + chunk-aware rebalance
  - grow / shrink / offload / retireCell linkage
```

---

## 13. Comparison with BigWorld / KBEngine / theseed

| Dimension | BigWorld | KBEngine | theseed |
|------|---------|---------|---------|
| BSP split move | Yes | No | Phase 3 goal |
| chunk-aware balance | Yes | No | Phase 3 goal |
| loadSafetyBound | Yes | Weak | Modeled explicitly |
| shouldOffload / retireCell linkage | Yes | Weak | Needs lifecycle/migration linkage |
| updateRanges separated from loadBalance | Yes | No | Now given its own boundary |
