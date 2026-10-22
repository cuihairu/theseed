# World Streaming & Compiled Space — Chunk, Geometry Mapping, and World Asset Boundaries

> English | **中文**

> Source of truth: BigWorld `SpaceData_MappingData / GeometryMapping / Chunk / CompiledSpace / space_converter`.
> KBEngine has no fully equivalent compiled space system and can only serve as a lightweight runtime space reference.
> This document does not bring in client rendering implementations; it only discusses their boundary implications for the server and the toolchain.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
BigWorld has a complete world asset chain:
  - Chunk / GeometryMapping
  - CompiledSpace
  - Conversion and packaging toolchain

The server does not fully understand rendering assets,
but it consumes key spatial mapping and boundary information.
```

### How KBEngine Implements It

```
KBEngine does not have as complete a compiled space system as BigWorld.
It leans more toward:
  - Runtime space
  - A thinner world asset layer
```

### Pros and Cons

```
BigWorld's advantages:
  - Complete linkage between world assets and the server-side space boundary
  - Suitable for large-world toolchains

KBEngine's advantages:
  - Lighter
  - Does not over-bind world editing assets into the engine trunk

Common disadvantages:
  - The world asset chain and the runtime boundary are easily written together in a mixed way
```

### theseed's Trade-offs

```
theseed explicitly places world streaming / compiled space at the world system layer,
does not mix it into the runtime Space definition,
but retains the BigWorld-level toolchain boundary as a long-term direction.
```

---

## 0. Design Boundary

```
This document is responsible for:
  - How world static assets are mapped to Space
  - The responsibility boundaries of chunk / geometry mapping / compiled space
  - Which world-side metadata the server needs to be aware of

This document is not responsible for:
  - WorldEditor UI interaction
  - Client rendering details
  - Specific asset formats and material systems
```

Relationship with existing documents:

```
01-space-topology-and-aoi
  - Covers the runtime Space abstraction and topology phases

02-navigation / 01-physics
  - Covers navigation and collision interfaces

This document
  - Covers "how world assets enter Space"
```

---

## 1. Why This Document Needs to Be Added

theseed's current `Space/Scene` documentation already clarifies:

```
single_cell
static_partition
bsp
```

the phase boundaries, but it still lacks the layer that is truly important on BigWorld's world side:

```
1. Space is not just an entity container
2. Geometry, collision, navigation, UDO, and static scene data are also mapped into the space
3. World assets are not scanned directly from directories at runtime; there is a mapping / compile / stream toolchain
```

Without adding this layer, the world system is easily miswritten as:

```
"Attach a navmesh file and a collision file to Space and you're done"
```

This is not enough to cover BigWorld's system boundary.

---

## 2. What Problem BigWorld Is Solving

As can be seen from `SpaceData_MappingData`, `GeometryMapping`, `Chunk`, and `CompiledSpace`, BigWorld actually splits four layers:

```
1. Space
   - World logic container

2. Geometry Mapping
   - The declaration that maps a set of geometric world assets into a Space

3. Chunk
   - The world is cut into addressable, streamable, queryable blocks

4. Compiled Space
   - Offline tools compile raw space assets into runtime-friendly data packages
```

The most important boundary here is not rendering, but rather:

```
World assets need to be prepared offline, not assembled temporarily at runtime
```

---

## 3. What theseed Covers Today

### 3.1 Covered

```
1. Space is independent of Entity
2. Space binds navigation and collision interfaces
3. topology has phased boundaries
```

### 3.2 Not Yet Covered

```
1. World asset mapping declaration model
2. Chunk-level streaming and query boundaries
3. Offline artifact boundaries of compiled world / compiled space
4. World metadata channels such as SpaceData
```

---

## 4. Core Concepts

### 4.1 Geometry Mapping

The responsibility of Geometry Mapping should be understood as:

```
Take a world geometric dataset
and, according to some transform,
mount it onto some Space
```

It is not an alias for "loading a model file".

### 4.2 Chunk

The responsibility of Chunk is:

```
Divide the large world into addressable, on-demand loadable, locally queryable static blocks
```

For the server, it at least affects:

```
  - Chunking of navigation data
  - Collision query scope
  - Loading boundaries of world objects / UDOs
  - Future multi-Cell topology and space load information
```

### 4.3 Compiled Space

The responsibility of Compiled Space is:

```
Convert editing-state world assets
into compiled artifacts that are efficient for runtime to read
```

The point is not the file format itself, but that it represents a clear boundary:

```
World preparation belongs to the offline toolchain
and is not the responsibility of Runtime Core
```

---

## 5. What the Server Actually Needs

The user's current audit does not count client rendering as a must-have item, so theseed does not need to port BigWorld's client-side CompiledSpace wholesale.

But the server still needs to define at least the following four categories of metadata:

### 5.1 Spatial Mapping Metadata

```
  - spaceId / sceneName
  - mapping source
  - transform
  - bounds
  - version
```

### 5.2 Navigation / Collision Metadata

```
  - navmesh package id
  - collision package id
  - Chunking boundaries
  - Version consistency
```

### 5.3 Static Object Metadata

```
  - static triggers
  - spawn markers
  - UDO / POI / region volumes
```

### 5.4 Streaming Metadata

```
  - Which chunks / partitions can be loaded on demand
  - Which must stay resident
  - Load dependencies and priorities
```

---

## 6. SpaceData-Type Problems Need a Boundary Set in Advance

BigWorld's `SPACE_DATA_MAPPING_KEY_*` shows that the world side does not rely only on configuration files.

A "space metadata channel" is also needed.

theseed does not have to copy BigWorld's binary key mechanism, but it needs to make clear:

```
Space Metadata Channel
  - Injects metadata such as geometry mapping / load bounds / world tags into Space
```

Suggested boundary:

```
What Runtime Core sees is:
  - Typed world metadata

Rather than:
  - Arbitrary string blobs
```

---

## 7. Recommended Data Model

### 7.1 World Mapping Description

```cpp
// world/WorldMappingDescriptor.h

struct WorldMappingDescriptor {
    std::string mappingId;
    std::string assetPackage;
    Matrix4x4 transform;
    AABB bounds;
    std::string version;
    bool serverRelevant = true;
};
```

### 7.2 Compiled Artifact Description

```cpp
// world/CompiledWorldDescriptor.h

struct CompiledWorldDescriptor {
    std::string worldId;
    std::string version;
    std::vector<std::string> chunkPackages;
    std::vector<std::string> navPackages;
    std::vector<std::string> collisionPackages;
};
```

### 7.3 Space Metadata Interface

```cpp
// world/ISpaceMetadataStore.h

class ISpaceMetadataStore {
public:
    virtual ~ISpaceMetadataStore() = default;

    virtual std::vector<WorldMappingDescriptor> mappings(
        SpaceId spaceId) const = 0;
    virtual std::optional<CompiledWorldDescriptor> compiledWorld(
        SpaceId spaceId) const = 0;
};
```

---

## 8. Relationship with topology

World streaming and topology are related, but they are not the same thing.

### 8.1 Do Not Merge Them into One Layer

```
topology
  - Cares about which Cell entities run on and how partitioning works

world streaming
  - Cares about how static world data is loaded into Space
```

### 8.2 Long-Term Coupling Points

By Phase 3, the two will interact:

```
1. chunk / partition boundaries may affect rebalance candidates
2. load bounds may participate in multi-Cell load decisions
3. The residency state of static world blocks may affect AOI and physics query cost
```

But this still does not mean the two should be tightly coupled now.

---

## 9. Relationship with Navigation / Physics

The server-side design should make clear:

```
CompiledWorld / Mapping
  ↓ Provides assets and boundaries

Navigation / Physics
  ↓ Consumes the prepared packages

Space Runtime
  ↓ Combines them
```

In other words:

```
Space should not be responsible for compiling world assets itself
```

---

## 10. These Capabilities Should Not Enter the MVP

According to theseed's current baseline, none of the following should be written as MVP capabilities:

```
1. Real chunk streaming
2. The compiled space asset compilation pipeline
3. Chunk-based dynamic world culling
4. Online hot mounting of world metadata
```

Their more reasonable boundary is:

```
Phase 1:
  - Single Space configuration + fixed navigation / collision packages

Phase 2:
  - Static partition + precompiled world description

Phase 3:
  - chunk streaming / compiled world pipeline / linkage with topology
```

---

## 11. theseed's Recommended Statement

It is recommended to write the world-side boundary uniformly as:

```
MVP:
  - Space references pre-generated navmesh / collision / static metadata
  - Does not promise chunk streaming
  - Does not promise a compiled world pipeline

Phase 2:
  - Introduce WorldMappingDescriptor / CompiledWorldDescriptor
  - Support precompiled world packages under static partitioning

Phase 3:
  - Support systematic capabilities for chunk / compiled space / geometry mapping
  - Link with multi-Cell topology, rebalance, and the world operations chain
```

---

## 12. Comparison with BigWorld / KBEngine / theseed

| Dimension | BigWorld | KBEngine | theseed |
|------|---------|---------|---------|
| Space independence | Strong | Weak, Space is more like an Entity | Covered |
| Geometry Mapping | Yes | Essentially none | Requires Phase 2/3 modeling |
| Chunk world | Yes | None | Phase 3 goal |
| Compiled Space | Has a complete toolchain | None | Not yet covered, needs a separate boundary |
| Offline compilation of world assets | Yes | Weak | Should be listed separately as a toolchain capability |
