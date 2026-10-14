# Space Topology & AOI — Space Topology, AOI Index, and Space Lifecycle

> **English** | [中文](01-space-topology-and-aoi.zh.md)

> Origin: BigWorld `CoordinateSystem / RangeTrigger / BSP topology / Space`.
> KBEngine is a lightweight reference implementation that inherits the AOI backbone but contracts Space into a path closer to a single Cell.
> theseed explicitly separates the runtime space, topology phases, and world asset boundary at the documentation level.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
BigWorld treats Space / Topology / AOI as the same runtime chain:
  - Space is the runtime container
  - Topology determines Cell distribution
  - AOI determines the visibility candidate set
  - Dynamic multi-Cell further connects to BSP / rebalance / chunk awareness
```

### How KBEngine Implements It

```
KBEngine inherits the AOI backbone and Space semantics,
but overall contracts more toward the single Space / single Cell route,
without as complete a dynamic multi-Cell system layer as BigWorld.
```

### Pros and Cons

```
BigWorld's advantages:
  - Complete space, AOI, and topology boundaries
  - Suitable for large-world dynamic scheduling

KBEngine's advantages:
  - Simple
  - Easier to first build out the runtime main path

Common disadvantages:
  - Once these three layers are written separately, the documentation easily becomes self-contradictory
```

### theseed's Trade-offs

```
theseed first puts Space / Topology / AOI back into the same layer,
the MVP selects single Cell + cross-linked list,
and explicitly places static partition and BSP into Phase 2 / 3.
```

---

## 0. Design Boundary

This document is responsible for:

```
  - Space's responsibilities as a runtime spatial container
  - AOI index selection
  - The phased model of ISpaceTopology
  - Space lifecycle and configuration boundary
```

This document is not responsible for:

```
  - Witness payload budget
  - Property Replication details
  - AoI Update priority curve
  - Load Bounds / BSP rebalance specific strategies
  - The offline asset boundary of Chunk / Geometry Mapping / Compiled Space
```

Related topics:

```
02-ghost-and-witness
03-runtime-communication-and-transport
04-property-replication
06-aoi-update-and-load-bounds
07-bsp-rebalance-and-offload
../6-world-and-game-framework/04-world-streaming-and-compiled-space
```

---

## 1. Why AOI and Space Are Placed Together

In engines like BigWorld / KBEngine, the following three things are actually one chain:

```
1. Space
   - Determines which spatial context an entity belongs to

2. Topology
   - Determines how this space is distributed across one or more Cells

3. AOI
   - Determines which entities within the same space are visible to each other
```

If they are split into unrelated documents, two kinds of errors easily arise:

```
1. Mistaking topology for a current capability
2. Mistaking AOI for an independent plugin unrelated to Space
```

Therefore, this round of reorganization places them back together in the "Replication and Space" layer.

---

## 2. Conceptual Comparison

| Dimension | BigWorld | KBEngine | theseed |
|------|------|------|------|
| Space nature | Independent management unit | Logically closer to single Cell Space | Independent management unit |
| Cell topology | BSP dynamic topology | Single Space single Cell | Phase 1 single Cell / Phase 2 static partition / Phase 3 BSP |
| AOI base structure | Cross-linked list + RangeTrigger | Cross-linked list + RangeTrigger | Cross-linked list + RangeTrigger |
| Chunk / Compiled Space | Yes | No complete equivalent layer | Placed separately in the world system layer |

The conclusion is not "theseed replicates all of BigWorld's world systems", but rather:

```
First get the boundaries of the runtime Space / Topology / AOI straight,
then decide which BigWorld world asset capabilities enter later phases.
```

---

## 3. AOI Index Selection

theseed continues to use the cross-linked list as the MVP AOI index.
This is not because it is the "most modern", but because it is stable enough in the target scenarios of BigWorld / KBEngine and can directly carry RangeTrigger semantics.

### 3.1 Candidate Comparison

| Scheme | Update Cost | Query Cost | Adapts to Uneven Density | Trigger Semantics | Conclusion |
|------|------|------|------|------|------|
| Cross-linked list | Stable | Moderate | Good | Natively supported | Suitable for this phase |
| Uniform Grid | Simple | Depends on cell size | Average | Requires extra wrapping | Can be a base scheme, but not prioritized |
| Spatial Hash | Simple to moderate | Depends on hash quality | Average | Requires extra wrapping | More of an engineering trick than a semantic-layer scheme |
| QuadTree | High | Depends on distribution | Good | Requires extra wrapping | Suitable for static or weakly dynamic scenarios |
| ECS Chunk + Grid | High | Good | Good | Strongly coupled to ECS | Suitable for ECS worlds, not the current main line |

### 3.2 How BigWorld Implements It

```
BigWorld's AOI is not a "simple radius query".
At its core it is:
  - RangeList / RangeTrigger maintains ordered boundaries on the coordinate axes
  - Witness subscribes to entities entering/leaving the candidate set
  - AOI radius changes, movement, and cross-Cell triggers all go through the same trigger chain

The value of this design is:
  - Updates and queries share one semantic chain
  - Triggers directly serve Witness / Ghost
  - Efficient enough for medium-radius visibility in MMOs
```

### 3.3 How KBEngine Implements It

```
KBEngine also inherits the cross-linked-list-style AOI backbone.
Its focus leans more toward:
  - A simplified single Space / single Cell model
  - Directly carrying EntityCall and Witness logic
  - Retaining range-change triggers, but without as complete a system-level periphery as BigWorld
```

### 3.4 Pros and Cons

```
Advantages of the cross-linked list:
  1. Stable update cost when entities move
  2. RangeTrigger can be attached directly to coordinate-axis boundaries
  3. Suitable for the medium-radius AOI queries common in MMOs
  4. Naturally matches the trigger semantics of Witness / Ghost
  5. Lower complexity than the dynamic balancing of tree structures

Disadvantages of the cross-linked list:
  1. More dependent on implementation details than a pure grid
  2. Not the optimal solution when density is extremely uneven
  3. Not the most natural index structure in a modern ECS world
```

### 3.5 theseed's Trade-offs

```
theseed still chooses the cross-linked list, not out of conservatism but because the boundary is clear:
  - The goal of this phase is to preserve BigWorld's Space / AOI semantics while borrowing KBEngine's lightweight main path
  - AOI must prioritize serving Witness / Ghost / Migration, rather than chasing the "newest" index terminology
  - Uniform Grid / Spatial Hash / QuadTree / ECS Chunk + Grid can all serve as later candidates, but should not preempt the current main line

Therefore the conclusion is:
  1. The MVP uses the cross-linked list
  2. In the future, if an ECS world or an extremely large sparse scenario is introduced, alternative indexes will be re-evaluated
  3. The documentation must be explicit: these alternative schemes are not currently defined as an implementation surface
```

### 3.6 Core Components

```cpp
class CoordinateSystem {
public:
    void insert(CoordinateNode* node);
    void remove(CoordinateNode* node);
    void update(CoordinateNode* node, const Vector3& newPos);

    void entitiesInRange(std::vector<Entity*>& out,
                         const Vector3& origin,
                         float radius,
                         uint16_t entityTypeFilter = 0) const;
};

class RangeTrigger {
public:
    RangeTrigger(Entity* owner, float range);
    virtual ~RangeTrigger() = default;

    void install(CoordinateSystem* coordSys);
    void uninstall();
    void updateRange(float newRange);

    virtual void onEnter(CoordinateNode* node) = 0;
    virtual void onLeave(CoordinateNode* node) = 0;
};
```

For now, only the "spatial index and triggers" are defined here.

Witness, detail level, and payload construction are not expanded in this document.

---

## 4. Space as a Runtime Container

Space in theseed should be defined as:

```
Runtime spatial context
  - Carries entity membership
  - Binds topology
  - Binds navigation / physics
  - Holds space-level data
```

Rather than:

```
A pure scene asset object
```

### 4.1 Core Interface

```cpp
class Space {
public:
    Space(SpaceId id, const std::string& name);

    void initialize(const SpaceConfig& config);
    void shutdown();

    void addEntity(Entity& entity);
    void removeEntity(EntityId id);
    Entity* findEntity(EntityId id) const;

    std::vector<Entity*> queryRange(const Vector3& center, float radius) const;
    uint32_t entityCount() const;

    INavigationSystem& navigation();
    IPhysicsQuery& physics();

    ISpaceTopology& topology();

    void setData(const std::string& key, const std::string& value);
    std::optional<std::string> getData(const std::string& key) const;
};
```

### 4.2 Lifecycle

```
create
  → load runtime bindings
  → activate
  → running
  → draining
  → shutdown
```

Among them:

```
Runtime topology initialization
  belongs to the Space lifecycle

chunk / compiled space offline asset preparation
  does not belong to this document's main line
```

---

## 5. The Phased Model of Topology

### 5.1 Phase 1: SingleCellTopology

```
  - One Space = one Cell
  - References KBEngine's lightweight single Cell path
  - Suitable for room-based, instance-based, and small-to-medium maps
```

### 5.2 Phase 2: StaticPartitionTopology

```
  - One Space = multiple fixed Cells
  - Partition boundaries are statically configured
  - Entities crossing partitions go through Ghost + Migration
```

### 5.3 Phase 3: BSPTopology

```
  - Benchmarks BigWorld's dynamic grow / shrink
  - Supports automatic rebalance / offload
  - Linked with load bounds, profiler feedback, and chunk awareness
```

### 5.4 Interface Expression

```cpp
class ISpaceTopology {
public:
    virtual ~ISpaceTopology() = default;

    virtual CellId locateCell(const Vector3& position) const = 0;
    virtual std::vector<CellId> getAdjacentCells(const Vector3& pos,
                                                 float radius) const = 0;
    virtual void onTopologyChanged(std::function<void()> callback) = 0;
    virtual void reportLoad(CellId cell, float load) = 0;
    virtual void rebalance() = 0;
};

class SingleCellTopology : public ISpaceTopology {};
class StaticPartitionTopology : public ISpaceTopology {};
class BSPTopology : public ISpaceTopology {};
```

The documentation must be explicit:

```
`topology="bsp"` is not a currently deliverable capability,
and is only allowed as a Phase 3 placeholder.
```

---

## 6. Configuration Boundary

The current document only treats `single_cell` as the MVP configuration.

```xml
<spaces>
    <space name="main_world" topology="single_cell">
        <navmesh layer="ground" path="navmesh/main_ground.nav"/>
        <collision path="collision/main.col"/>
        <bounds minX="-5000" maxX="5000" minZ="-5000" maxZ="5000"/>
    </space>
</spaces>
```

Future placeholders may be retained, but the phase must be marked separately:

```xml
<!-- Phase 2 -->
<space name="main_world" topology="static_partition">
    <partition rows="2" cols="2"/>
</space>

<!-- Phase 3 -->
<space name="continent_01" topology="bsp">
    <rebalance policy="auto"/>
</space>
```

The purpose of writing it this way is:

```
Preserve the design abstraction,
but do not disguise long-term capabilities as the current implementation surface.
```

---

## 7. Interfaces with Other System Areas

### 7.1 With Ghost / Witness

```
AOI determines "who enters the view candidate set"
Witness determines "how these candidate sets become client-visible state"
```

### 7.2 With Runtime Transport

```
Space / Topology determines the entity routing context
Runtime Transport is responsible for sending cross-Cell runtime messages out
```

### 7.3 With Load Bounds / BSP Rebalance

```
Space topology is the carrier
load bounds / rebalance is the scheduling policy
```

### 7.4 With World Streaming / Compiled Space

```
Space runtime
  != chunk asset pipeline
```

In BigWorld these two layers are related, but they are not one abstraction.

---

## 8. Phased Boundary

```
MVP:
  - single_cell
  - Cross-linked list AOI
  - Basic Space lifecycle

Phase 2:
  - static_partition
  - Fixed-boundary multi-Cell
  - Linked with the ghost / migration main path

Phase 3:
  - BSP topology
  - grow / shrink
  - Linked with load bounds, rebalance, and chunk awareness
```

---

## 9. One-Sentence Judgment

The core message this document intends to express is not:

```
theseed already has a BigWorld-style dynamic world
```

But rather:

```
theseed has already put the Space / AOI / Topology abstractions back into the same layer,
and has clarified the boundary differences between it and BigWorld's world systems and KBEngine's single Cell path.
```
