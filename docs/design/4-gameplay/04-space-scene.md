# Space & Scene — 空间与场景管理

> Space 是 theseed 的空间管理单元，绑定导航网格和碰撞数据。
>
> 来源：BigWorld Space+Chunk+BSP（动态加载），KBEngine Space=Entity（简化）。
> theseed 取 BigWorld 的 Space 独立管理 + ISpaceTopology 可插拔拓扑。

---

## 0. 分阶段边界

```
MVP / Phase 1：
  - 只支持 single_cell
  - 不承诺 BigWorld 级 BSP 动态拓扑
  - 不承诺 chunk 级动态流送

Phase 2：
  - 支持 static_partition
  - 支持固定边界多 Cell

Phase 3：
  - 才考虑 BSP / grow-shrink / auto rebalance
  - 才考虑真正的 chunk stream
```

因此本文所有 `bsp` 相关概念都应理解为：

```
远期设计占位
不是当前可兑现能力
```

世界资产映射、chunk stream、compiled space 的边界见：

`./07-world-streaming-and-compiled-space.md`

---

## 1. 概念对比

```
BigWorld:  Space → 多个 Cell → Chunk 动态加载/卸载 → BSP 动态拓扑
KBEngine:  Space = Entity → 单 Cell → 全量加载
theseed:   Phase 1 单 Cell → Phase 2 静态分区 → Phase 3 动态拓扑
```

---

## 2. Space 设计

```cpp
class Space {
public:
    Space(SpaceId id, const std::string& name);

    // 生命周期
    void initialize(const SpaceConfig& config);
    void shutdown();

    // 实体管理
    void addEntity(Entity& entity);
    void removeEntity(EntityId id);
    Entity* findEntity(EntityId id) const;

    // 查询
    std::vector<Entity*> queryRange(const Vector3& center, float radius) const;
    uint32_t entityCount() const;

    // 导航 & 物理
    INavigationSystem& navigation();
    IPhysicsQuery& physics();

    // 拓扑
    ISpaceTopology& topology();
    const CellInfo* cellAt(float x, float z) const;

    // Space 数据
    void setData(const std::string& key, const std::string& value);
    std::optional<std::string> getData(const std::string& key) const;

private:
    SpaceId id_;
    std::unique_ptr<ISpaceTopology> topology_;
    std::unique_ptr<INavigationSystem> navigation_;
    std::unique_ptr<IPhysicsQuery> physics_;
    std::unordered_map<EntityId, Entity*> entities_;
};
```

---

## 3. Space 生命周期

```
创建 → 加载场景 → 运行 → 卸载场景 → 销毁

1. Space::create("dungeon_01")
   → 分配 SpaceId
   → 加载 NavMesh / 碰撞网格
   → 初始化拓扑
   → onSpaceCreated(spaceId)

2. Entity 进入 Space
   → space.addEntity(entity)
   → entity.onEnterSpace(spaceId)
   → AOI 注册

3. Entity 离开 Space
   → space.removeEntity(entityId)
   → entity.onLeaveSpace(spaceId)

4. Space::shutdown()
   → 迁移/销毁所有实体
   → 卸载 NavMesh / 碰撞网格
   → onSpaceDestroyed(spaceId)
```

---

## 4. Space 配置

```xml
<spaces>
    <!-- MVP / Phase 1 -->
    <space name="main_world" topology="single_cell">
        <navmesh layer="ground" path="navmesh/main_ground.nav"/>
        <collision path="collision/main.col"/>
        <bounds minX="-5000" maxX="5000" minZ="-5000" maxZ="5000"/>
    </space>

    <space name="dungeon_01" topology="single_cell">
        <navmesh path="navmesh/dungeon_01.nav"/>
        <collision path="collision/dungeon_01.col"/>
    </space>
</spaces>
```

未来配置占位：

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

---

## 5. 对比

| 能力 | BigWorld | KBEngine | theseed |
|------|----------|----------|---------|
| Space 本质 | 独立管理单元 | 继承 Entity | 独立管理单元 |
| Cell 分配 | BSP 动态拓扑 | 单 Cell | Phase 1 单 Cell / Phase 2 静态分区 / Phase 3 BSP |
| Chunk 加载 | 动态加载/卸载 | 无 | MVP 不承诺，Phase 3 再评估 |
| NavMesh 绑定 | per-Space | per-Space | per-Space |
| 动态创建 | 支持 | 支持 | 支持 |
