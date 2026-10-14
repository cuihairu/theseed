# Navigation — Navigation and Pathfinding

> **English** | [中文](02-navigation.zh.md)

> Recast/Detour is the industry-standard pathfinding engine, used by both BigWorld and KBEngine.
> theseed integrates it directly and adds dynamic obstacle and multi-layer NavMesh support.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
BigWorld directly incorporates the navigation system into the space runtime:
  - NavMesh queries
  - Linked with space geometry and chunk assets
  - Serves AI, movement, and physics validation
```

### How KBEngine Implements It

```
KBEngine also uses the Recast/Detour route,
but its overall world system support is lighter than BigWorld's.
```

### Pros and Cons

```
Common advantages:
  - Mature in the industry
  - High adaptability with server-side movement/AI

Common disadvantages:
  - Dynamic obstacles and multi-layer navigation increase complexity
  - Still depends on world asset quality
```

### theseed's Trade-offs

```
theseed keeps the Recast/Detour main line,
but writes dynamic obstacles and multi-layer support as explicit extension items,
and does not mix world editor semantics into the runtime documentation.
```

---

## 1. INavigationSystem Interface

```cpp
class INavigationSystem {
public:
    // 寻路
    virtual Path findPath(const Vector3& start, const Vector3& end,
                          const NavQueryFilter& filter = {}) = 0;

    // 辅助查询
    virtual Vector3 findRandomPointAround(const Vector3& center, float radius, ...) = 0;
    virtual NavRaycastResult raycast(const Vector3& start, const Vector3& end, ...) = 0;
    virtual std::optional<Vector3> snapToNav(const Vector3& position) = 0;

    // NavMesh 管理
    virtual Future<void> loadNavMesh(SpaceId spaceId, const std::string& navMeshPath) = 0;
    virtual void unloadNavMesh(SpaceId spaceId) = 0;

    // 动态障碍物
    virtual ObstacleHandle addObstacle(const Vector3& pos, float radius, float height) = 0;
    virtual void removeObstacle(ObstacleHandle handle) = 0;
};
```

---

## 2. Script Layer API

```python
class Monster(BaseEntity):
    def patrol(self):
        target = theseed.navigation.findRandomPointAround(self.position, radius=30.0)
        self.navigateTo(target, speed=5.0)

    def chaseTarget(self, targetId):
        target = theseed.getEntity(targetId)
        path = theseed.navigation.findPath(self.position, target.position)
        if path and path.totalDistance < 50.0:
            self.followPath(path.waypoints, speed=8.0)
```

---

## 3. Binding NavMesh with Space

```xml
<spaces>
    <space name="main_world">
        <navmesh layer="ground" path="navmesh/main_ground.nav"/>
        <navmesh layer="water" path="navmesh/main_water.nav"/>
        <navmesh layer="air" path="navmesh/main_air.nav"/>
    </space>
</spaces>
```

NavMesh is bound one-to-one with Space: loaded when the Space is created, unloaded when it is destroyed.

---

## 4. Comparison

| Capability | BigWorld | KBEngine | theseed |
|------|----------|----------|---------|
| Pathfinding engine | Recast + AStar | Recast/Detour | Recast/Detour |
| Dynamic obstacles | None | None | addObstacle/removeObstacle() |
| Multi-layer NavMesh | Single layer | Multi-layer | Multi-layer |
| Asynchronous loading | None | loadnavmesh_threadtasks | Future<void> |
