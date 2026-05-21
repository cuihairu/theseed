# Physics — 服务端物理与碰撞

> 服务端物理 ≠ 客户端渲染物理。服务端只做游戏规则验证：射线检测、范围判定、碰撞查询。
>
> 来源：BigWorld Physics2（完整服务端物理），KBEngine 无服务端物理。

---

## 1. 为什么服务端需要物理

```
服务端物理的刚需场景：
  1. 射线检测：技能弹道是否命中？视线是否被遮挡？
  2. 范围判定：AOE 技能影响哪些实体？
  3. 碰撞查询：角色能不能穿墙？移动是否合法？
  4. 掉落检测：物品掉落在地面哪个位置？
  5. NavMesh 依赖：寻路需要碰撞几何体
```

---

## 2. IPhysicsQuery 接口

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

## 3. 脚本层 API

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

## 4. 对比

| 能力 | BigWorld | KBEngine | theseed |
|------|----------|----------|---------|
| 射线检测 | Physics2 collide() | 无 | IPhysicsQuery::raycast() |
| 范围查询 | QuadTree | 无 | overlapSphere/Box() |
| 视线检测 | raycast with callback | 无 | lineOfSight() |
| 碰撞数据 | WorldTriangle + QuadTree | 无 | NavMesh + 碰撞网格 |
| 物理模拟 | 无（服务端不模拟） | 无 | 不做 |
