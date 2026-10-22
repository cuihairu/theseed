# Physics — Server-Side Physics and Collision

> English | **中文**

> Server-side physics ≠ client rendering physics. The server only performs game rule validation: raycasts, overlap checks, collision queries.
>
> Source of truth: BigWorld Physics2's complete server-side physics boundary.
> KBEngine essentially has no complete server-side physics layer and can only serve as a lightweight gap reference.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
BigWorld has a complete server-side physics subsystem:
  - Physics queries
  - Collision body management
  - Linked with space and Chunk assets
```

### How KBEngine Implements It

```
KBEngine essentially has no complete server-side physics layer,
and usually relies more on script logic, simplified geometry, and external components.
```

### Pros and Cons

```
BigWorld's advantages:
  - Can push rule validation down to the engine layer
  - Tighter coordination with space and navigation systems

KBEngine's advantages:
  - Lightweight
  - One less layer of system complexity

Common disadvantages:
  - Truly complete server-side physics is very costly
```

### theseed's Trade-offs

```
theseed only retains the server-side physics query boundary
and does not make a complete Physics2 an MVP goal.
```

---

## 1. Why the Server Needs Physics

```
Hard requirements for server-side physics:
  1. Raycasts: did the skill projectile hit? Is the line of sight blocked?
  2. Overlap checks: which entities are affected by an AOE skill?
  3. Collision queries: can a character walk through walls? Is the movement legal?
  4. Drop detection: where on the ground does an item land?
  5. NavMesh dependency: pathfinding requires collision geometry
```

---

## 2. IPhysicsQuery Interface

```cpp
class IPhysicsQuery {
public:
    // 射线检测
    virtual RaycastResult raycast(const Vector3& src, const Vector3& dir,
                                  float maxDistance, CollisionFilter filter = {}) = 0;
    virtual RaycastResult raycastBetween(const Vector3& src, const Vector3& dst,
                                         CollisionFilter filter = {}) = 0;

    // 范围查询
    virtual std::vector<OverlapResult> overlapSphere(const Vector3& center, float radius,
                                                      CollisionFilter filter = {}) = 0;
    virtual std::vector<OverlapResult> overlapBox(const Vector3& center, const Vector3& halfExtents,
                                                   CollisionFilter filter = {}) = 0;

    // 可见性检测
    virtual bool lineOfSight(const Vector3& from, const Vector3& to) = 0;

    // 地面投影
    virtual std::optional<Vector3> projectToGround(const Vector3& position, float maxDrop = 1000.f) = 0;

    // 导航区域检测
    virtual bool isNavigable(const Vector3& position) = 0;
};
```

---

## 3. Script Layer API

```python
import theseed.physics

class Avatar(BaseEntity):
    def attack(self, targetId, skillId):
        target = theseed.getEntity(targetId)
        if not theseed.physics.lineOfSight(self.position, target.position):
            return  # 被墙壁遮挡

        if skillId == SKILL_FIREBALL:
            results = theseed.physics.overlapSphere(
                target.position, radius=5.0,
                filter={"includeTags": ["enemy"]}
            )
            for hit in results:
                self.dealDamage(hit.entityId, calculateDamage(skillId))

    def move(self, position, rotation):
        if not theseed.physics.isNavigable(position):
            return
        result = theseed.physics.raycastBetween(self.position, position)
        if result.hit:
            return  # 路径上有障碍物
        self.position = position
```

---

## 4. Comparison

| Capability | BigWorld | KBEngine | theseed |
|------|----------|----------|---------|
| Raycast | Physics2 collide() | None | IPhysicsQuery::raycast() |
| Overlap query | QuadTree | None | overlapSphere/Box() |
| Line of sight | raycast with callback | None | lineOfSight() |
| Collision data | WorldTriangle + QuadTree | None | NavMesh + collision mesh |
| Physics simulation | None (server does not simulate) | None | Not doing |
