# Navigation — 导航与寻路

> Recast/Detour 是行业标准寻路引擎，BigWorld 和 KBEngine 都使用。
> theseed 直接集成，增加动态障碍物和多层 NavMesh 支持。

---

## 1. INavigationSystem 接口

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

## 2. 脚本层 API

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

## 3. NavMesh 与 Space 绑定

```xml
<spaces>
    <space name="main_world">
        <navmesh layer="ground" path="navmesh/main_ground.nav"/>
        <navmesh layer="water" path="navmesh/main_water.nav"/>
        <navmesh layer="air" path="navmesh/main_air.nav"/>
    </space>
</spaces>
```

NavMesh 与 Space 一一绑定：Space 创建时加载，销毁时卸载。

---

## 4. 对比

| 能力 | BigWorld | KBEngine | theseed |
|------|----------|----------|---------|
| 寻路引擎 | Recast + AStar | Recast/Detour | Recast/Detour |
| 动态障碍 | 无 | 无 | addObstacle/removeObstacle() |
| 多层 NavMesh | 单层 | 多层 | 多层 |
| 异步加载 | 无 | loadnavmesh_threadtasks | Future<void> |
