# AoI Update Schemes & Load Bounds — Update Priority Curves and Spatial Load Bounds

> **English** | [中文](06-aoi-update-and-load-bounds.zh.md)

> Origin: BigWorld `cellapp/aoi_update_schemes`, `Witness / EntityCache priority`, `SPACE_DATA_SERVER_LOAD_BOUNDS`, `cellappmgr::Space::updateRanges`.
> KBEngine has only thin reference value at this layer.
> What this document adds is BigWorld's systematized boundary between AOI and spatial load, not another explanation of the cross-linked list.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
BigWorld links AOI updates with spatial load boundaries:
  - Adjusts entity update frequency through priority / update scheme
  - Feeds back to Cell partitioning and range scheduling through load bounds
  - It is not simply "querying neighbors", but "who updates first and who updates later"
```

### How KBEngine Implements It

```
KBEngine also has AOI and view updates, but the overall chain is shorter:
  - Centered on Witness / triggers
  - Without as strong a system-level load feedback layer as BigWorld
```

### Pros and Cons

```
BigWorld's advantages:
  - Scheduling and spatial load are combined more tightly
  - Suitable for large-world dynamic management

KBEngine's advantages:
  - Simple
  - Easier to first build out the main path

Common disadvantages:
  - The scheme is complex, and the boundaries must be written very clearly
```

### theseed's Trade-offs

```
theseed keeps only the interface boundary of "update scheme + load bounds",
without writing it as a currently completed dynamic partitioning capability,
avoiding disguising the content of Phase 2 / 3 as the MVP.
```

---

## 0. Design Boundary

```
This document is responsible for:
  - The real responsibilities of the AoI update scheme
  - The update priority curve of each visible entity
  - load bounds / range update and the spatial load boundary
  - The interface relationship between AoI update frequency and spatial partition load

This document is not responsible for:
  - The cross-linked list index itself
  - Property synchronization protocol encoding
  - Complete BSP topology implementation details
```

Relationship with other documents:

```
01-space-topology-and-aoi
  - Discusses the AOI index and the main body of topology

04-property-replication
  - Discusses how properties are sent

This document
  - Discusses "when to send to whom faster/slower"
  - Discusses how the spatial load boundary feeds back into Cell partitioning
```

---

## 1. First Correct a Common Misreading

BigWorld's `aoi_update_schemes` is not "a complete AOI plugin framework for different game types".

Its more precise responsibility is:

```
For a given entity in a Witness's view,
define a distance-based update priority change curve.
```

In other words, what it controls is:

```
EntityCache priority delta
```

Rather than:

```
Whether CoordinateSystem exists
Whether AOI is changed to room broadcast
How the entire visibility model is replaced
```

This is also where the previous theseed documentation was too broad.

---

## 2. What BigWorld Is Actually Doing

From `AoIUpdateScheme::init/apply` and `Witness::update`, BigWorld's approach can be seen as:

```
1. Every visible entity has an EntityCache
2. Every EntityCache has an updateSchemeID
3. The scheme applies a delta to priority based on distance
4. Witness sends only a subset of entities within the priority window each tick
```

This means BigWorld has two layers on AOI:

```
Layer A: who is in the view
  - AOI enter / leave

Layer B: among those in the view, who is sent first and how often
  - update scheme + priority queue + bandwidth budget
```

The current theseed already covers the main body of Layer A, but Layer B has not yet been given its own boundary.

---

## 3. The Original Meaning of AoI Update Scheme

### 3.1 Responsibilities

What AoI Update Scheme describes is:

```
distance
  → priority delta
```

BigWorld's configuration is essentially two control points:

```
minDelta
maxDelta
```

Then combined with `maxAoIRadius` to derive a linear curve.

### 3.2 Special Semantics

There is also an important special case in BigWorld:

```
minDelta = 0
maxDelta = 0
```

This means:

```
Treat the target as coincident
i.e. "handle it at the highest detail and highest priority"
```

This is not another scheme name, but an explicit priority semantics.

---

## 4. What It Solves Is Not "Visibility", but "Budget Allocation"

The key problem of Witness is not merely knowing who is visible, but rather:

```
Within one tick / packet budget,
which entities to update first.
```

BigWorld's approach can be abstracted as:

```
visible entities
  → priority queue
  → update scheme changes priority delta over distance
  → cap max priority delta each tick
  → fit into desired packet size
```

So what theseed needs to add is not "yet another AOI policy interface", but rather:

```
AoI update priority policy
```

---

## 5. How theseed Should Model This

### 5.1 Modeling Goals

theseed needs to satisfy simultaneously:

```
1. Do not lose BigWorld's per-entity priority update semantics
2. Do not mistakenly abstract the entire AOI system into "an arbitrary plugin"
3. Leave a differentiated configuration entry point for future MMO / high-frequency combat scenarios
```

### 5.2 Recommended Abstraction

```cpp
// aoi/IAoIUpdatePolicy.h

using AoIUpdatePolicyId = uint16_t;

struct AoIUpdatePolicyConfig {
    std::string name;
    float minPriorityDelta = 1.0f;
    float maxPriorityDelta = 5.0f;
    bool treatAsCoincident = false;
};

class IAoIUpdatePolicy {
public:
    virtual ~IAoIUpdatePolicy() = default;

    virtual std::string name() const = 0;
    virtual double priorityDelta(float distanceMeters) const = 0;
    virtual bool isCoincident() const = 0;
};
```

### 5.3 Usage Point on the Witness Side

```cpp
// aoi/WitnessPriorityScheduler.h

class IWitnessPriorityScheduler {
public:
    virtual ~IWitnessPriorityScheduler() = default;

    virtual void setPolicy(EntityId entityId, AoIUpdatePolicyId policyId) = 0;
    virtual void onDistanceChanged(EntityId entityId, float distanceMeters) = 0;
    virtual std::vector<EntityId> selectForUpdate(
        size_t packetBudgetBytes,
        float maxPriorityDeltaPerTick) = 0;
};
```

Design requirements:

```
1. policy controls priority, not visibility directly
2. policy granularity is "the update strategy of a target entity within a given witness"
3. scheduler is responsible for the budget window, and policy is not allowed to operate the bundle directly
```

---

## 6. Do Not Mix the Three Things Together Again

AOI design must continuously keep three things separate:

| Layer | Question | BigWorld Representative Mechanism |
|------|------|------------------|
| Visibility | Who enters/leaves the view | CoordinateSystem / RangeTrigger |
| Priority | Who is sent first in the view | AoIUpdateScheme / EntityCache priority |
| Payload | Which fields are sent | detail level / property sync |

If the three are mixed into one `IAOIScheme`, it will lead to:

```
1. Distortion of BigWorld's original meaning
2. Synchronization strategy, priority strategy, and visibility strategy become dead-coupled
3. The Runtime Core quickly grows a "capability bucket interface"
```

---

## 7. Load Bounds Is Another System Line

BigWorld's `SPACE_DATA_SERVER_LOAD_BOUNDS` and `updateRanges()` show that:

```
Space does not only have entity load;
there also exists a "spatial load boundary that the server should pay attention to".
```

The problem this layer solves is:

```
In multi-Cell / BSP scenarios,
which regions' load, boundaries, and chunk coverage should participate in partition updates.
```

Therefore load bounds should not be written as:

```
An ordinary SpaceData key
```

But should be regarded as:

```
Input metadata for Topology / balancing
```

---

## 8. What BigWorld's Spatial Load Boundary Is Doing

From `cellappmgr::Space::loadBalance / updateRanges` and `CellData::balanceChunkBounds`, several points can be extracted:

```
1. Spatial boundary updates do not only look at the current Cell's entity count
2. They also consider chunk coverage, unloaded regions, and ghost distance adjacency cost
3. loadSafetyBound restricts further enlarging an already overloaded region during balancing
4. range update and load balance are related, but they are not the same thing
```

This part is exactly where the current theseed only writes phase boundaries and does not write system semantics.

---

## 9. theseed's Recommended Modeling

### 9.1 Load Boundary Description

```cpp
// topology/SpaceLoadBounds.h

struct SpaceLoadBounds {
    Rect2f logicalBounds;
    Rect2f loadedChunkBounds;
    Rect2f serverInterestBounds;
    float ghostMarginMeters = 0.0f;
};
```

### 9.2 Partition Update Interface

```cpp
// topology/ISpaceRangeUpdater.h

struct SpaceRangeUpdateInput {
    SpaceId spaceId;
    SpaceLoadBounds bounds;
    float avgSmoothedLoad = 0.0f;
    float maxSmoothedLoad = 0.0f;
    float loadSafetyBound = 0.0f;
};

class ISpaceRangeUpdater {
public:
    virtual ~ISpaceRangeUpdater() = default;

    virtual void updateRanges(const SpaceRangeUpdateInput& input) = 0;
};
```

### 9.3 Design Requirements

```
1. load bounds is a topology input, not gameplay configuration sugar
2. updateRanges can be executed independently of a complete rebalance
3. chunk / load bounds / ghost margin must be observable
```

---

## 10. Relationship with World Streaming

This document is directly related to [04-world-streaming-and-compiled-space](../6-world-and-game-framework/04-world-streaming-and-compiled-space.md), but the boundaries differ:

```
world streaming
  - Focuses on how static world assets enter the Space

load bounds
  - Focuses on how these assets/boundaries affect partitioning and balancing
```

Only when these two documents are combined do they come closer to BigWorld's world-side system semantics.

---

## 11. Phased Boundary

```
MVP:
  - Keep only the default AoI update priority policy
  - Do not commit to per-entity custom strategies
  - Do not commit to load bounds-driven range update

Phase 2:
  - Support overriding the update policy by entity or entity type
  - Introduce the SpaceLoadBounds abstraction
  - Support basic updateRanges under static partitioning

Phase 3:
  - Support a per-entity priority curve closer to BigWorld's
  - Support dynamic range updates driven by load bounds / chunk bounds / ghost margin
  - Link with BSP rebalance
```

---

## 12. Comparison with BigWorld / KBEngine / theseed

| Dimension | BigWorld | KBEngine | theseed |
|------|---------|---------|---------|
| visible set | Yes | Yes | Already covered |
| per-entity update scheme | Yes | Essentially none | Phase 2/3 goal |
| coincident high-priority semantics | Yes | Weak | Should be modeled explicitly |
| load bounds / range update | Yes | Weak | Needs a separate boundary |
| AOI / priority / payload layering | Clear but scattered | Rather mixed | Now beginning to be split explicitly |
