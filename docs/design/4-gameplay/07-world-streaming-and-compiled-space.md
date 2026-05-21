# World Streaming & Compiled Space — Chunk、Geometry Mapping 与世界资产边界

> 来源：BigWorld `SpaceData_MappingData / GeometryMapping / Chunk / CompiledSpace / space_converter`。
> 本篇不把客户端渲染实现拉进来，只讨论它们对服务端与工具链的边界含义。

---

## 0. 设计边界

```
本篇负责：
  - 世界静态资产如何映射到 Space
  - chunk / geometry mapping / compiled space 的职责边界
  - 服务端需要感知到哪些世界侧元数据

本篇不负责：
  - WorldEditor UI 交互
  - 客户端渲染细节
  - 具体资源格式和材质系统
```

和现有文档的关系：

```
04-space-scene
  - 讲运行时 Space 抽象和 topology phase

02-navigation / 01-physics
  - 讲导航和碰撞接口

本篇
  - 讲“世界资产如何进入 Space”
```

---

## 1. 为什么要补这一篇

当前 theseed 的 `Space/Scene` 文档已经把：

```
single_cell
static_partition
bsp
```

的阶段边界写清了，但还缺 BigWorld 世界侧真正重要的一层：

```
1. Space 不只是个实体容器
2. 空间里还会映射几何、碰撞、导航、UDO、静态场景数据
3. 世界资产不是运行时直接扫目录，而是有 mapping / compile / stream 的工具链
```

不补这层，就容易把世界系统误写成：

```
“给 Space 挂一个 navmesh 文件和 collision 文件就结束”
```

这不足以覆盖 BigWorld 的系统边界。

---

## 2. BigWorld 在解决什么问题

从 `SpaceData_MappingData`、`GeometryMapping`、`Chunk`、`CompiledSpace` 可以看出，BigWorld 实际拆了四层：

```
1. Space
   - 世界逻辑容器

2. Geometry Mapping
   - 把一组几何世界资产映射进 Space 的声明

3. Chunk
   - 世界被切成可寻址、可流送、可查询的块

4. Compiled Space
   - 由离线工具把原始空间资产编译成运行时友好的数据包
```

其中最重要的边界不是渲染，而是：

```
世界资产需要离线准备，而不是运行时临时拼接
```

---

## 3. theseed 现在覆盖到哪里

### 3.1 已覆盖

```
1. Space 独立于 Entity
2. Space 绑定导航与碰撞接口
3. topology 有分阶段边界
```

### 3.2 还没覆盖

```
1. 世界资产映射声明模型
2. chunk 级流送与查询边界
3. compiled world / compiled space 的离线产物边界
4. SpaceData 一类世界元数据通道
```

---

## 4. 核心概念

### 4.1 Geometry Mapping

Geometry Mapping 的职责应理解为：

```
把一份世界几何数据集
按某个 transform
挂载到某个 Space
```

它不是“加载一个模型文件”的别名。

### 4.2 Chunk

Chunk 的职责是：

```
把大世界分成可寻址、可按需加载、可局部查询的静态块
```

对服务端而言，它至少会影响：

```
  - 导航数据的分块
  - 碰撞查询范围
  - world object / UDO 的加载边界
  - 远期多 Cell 拓扑与空间负载信息
```

### 4.3 Compiled Space

Compiled Space 的职责是：

```
把编辑态世界资产
转成运行时高效读取的编译产物
```

重点不是文件格式本身，而是它代表一种明确边界：

```
世界准备属于离线工具链
不是 Runtime Core 的职责
```

---

## 5. 服务端真正需要什么

用户这次审计不把客户端渲染算入必做项，所以 theseed 不需要把 BigWorld 客户端 CompiledSpace 全盘搬进来。

但服务端仍然需要至少定义下面四类元数据：

### 5.1 空间映射元数据

```
  - spaceId / sceneName
  - mapping source
  - transform
  - bounds
  - version
```

### 5.2 导航 / 碰撞元数据

```
  - navmesh package id
  - collision package id
  - 分块边界
  - 版本一致性
```

### 5.3 静态对象元数据

```
  - static triggers
  - spawn markers
  - UDO / POI / region volumes
```

### 5.4 流送元数据

```
  - 哪些 chunk / partition 可以按需加载
  - 哪些必须常驻
  - 加载依赖与优先级
```

---

## 6. SpaceData 类问题必须提前立边界

BigWorld 的 `SPACE_DATA_MAPPING_KEY_*` 说明世界侧并不只靠配置文件。

还需要一条“空间元数据通道”。

theseed 可以不照搬 BigWorld 的二进制 key 机制，但需要立清楚：

```
Space Metadata Channel
  - 向 Space 注入 geometry mapping / load bounds / world tags 等元数据
```

建议边界：

```
Runtime Core 看到的是：
  - typed world metadata

而不是：
  - 任意字符串 blob
```

---

## 7. 推荐的数据模型

### 7.1 世界映射描述

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

### 7.2 编译产物描述

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

### 7.3 空间元数据接口

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

## 8. 和 topology 的关系

世界流送与 topology 有关联，但不是同一件事。

### 8.1 不要混成一层

```
topology
  - 关心实体运行在哪个 Cell、如何分区

world streaming
  - 关心静态世界数据如何装入 Space
```

### 8.2 远期耦合点

到了 Phase 3，二者会发生联动：

```
1. chunk / partition 边界可能影响 rebalance 候选
2. load bounds 可能参与多 Cell 负载决策
3. 静态世界块的驻留状态可能影响 AOI 与物理查询成本
```

但这仍不意味着现在就要把两者耦死。

---

## 9. 和导航 / 物理的关系

服务端设计上建议明确：

```
CompiledWorld / Mapping
  ↓ 提供资产和边界

Navigation / Physics
  ↓ 消费已准备好的包

Space Runtime
  ↓ 组合它们
```

也就是说：

```
Space 不应自己负责编译世界资产
```

---

## 10. 这些能力不该进 MVP

按当前 theseed 的基线，下面这些都不应写成 MVP 能力：

```
1. 真正的 chunk stream
2. compiled space 资产编译流水线
3. 基于 chunk 的动态世界裁剪
4. world metadata 的在线热挂载
```

它们更合理的边界是：

```
Phase 1：
  - 单一 Space 配置 + 固定导航 / 碰撞包

Phase 2：
  - 静态 partition + 预编译世界描述

Phase 3：
  - chunk stream / compiled world pipeline / 与 topology 联动
```

---

## 11. theseed 的推荐表述

建议把世界侧边界统一写成：

```
MVP：
  - Space 引用预生成的 navmesh / collision / static metadata
  - 不承诺 chunk stream
  - 不承诺 compiled world pipeline

Phase 2：
  - 引入 WorldMappingDescriptor / CompiledWorldDescriptor
  - 支持静态分区下的预编译世界包

Phase 3：
  - 支持 chunk / compiled space / geometry mapping 的系统化能力
  - 与多 Cell topology、rebalance、世界运维链联动
```

---

## 12. 与 BigWorld / KBEngine / theseed 的对比

| 维度 | BigWorld | KBEngine | theseed |
|------|---------|---------|---------|
| Space 独立性 | 强 | 弱，Space 更像 Entity | 已覆盖 |
| Geometry Mapping | 有 | 基本无 | 需 Phase 2/3 建模 |
| Chunk world | 有 | 无 | Phase 3 目标 |
| Compiled Space | 有完整工具链 | 无 | 暂未覆盖，需单列边界 |
| 世界资产离线编译 | 有 | 弱 | 应单列为工具链能力 |
