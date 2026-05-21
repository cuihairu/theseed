# Space & Scene — 空间与场景管理

> Space 是 theseed 的空间管理单元，绑定导航网格和碰撞数据。
>
> 来源：BigWorld Space+Chunk+BSP（动态加载），KBEngine Space=Entity（简化）。
> theseed 取 BigWorld 的 Space 独立管理 + ISpaceTopology 可插拔拓扑。

---

## 1. 概念对比

```
BigWorld:  Space → 多个 Cell → Chunk 动态加载/卸载 → BSP 动态拓扑
KBEngine:  Space = Entity → 单 Cell → 全量加载
theseed:   Space = 独立管理单元 → ISpaceTopology 可插拔 → 按需加载
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
    <space name="main_world" topology="bsp">
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

---

## 5. 对比

| 能力 | BigWorld | KBEngine | theseed |
|------|----------|----------|---------|
| Space 本质 | 独立管理单元 | 继承 Entity | 独立管理单元 |
| Cell 分配 | BSP 动态拓扑 | 单 Cell | ISpaceTopology 可插拔 |
| Chunk 加载 | 动态加载/卸载 | 无 | 按需加载 |
| NavMesh 绑定 | per-Space | per-Space | per-Space |
| 动态创建 | 支持 | 支持 | 支持 |
