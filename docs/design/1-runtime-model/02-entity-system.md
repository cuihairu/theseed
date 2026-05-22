# Entity System — 实体系统

> theseed 实体的核心设计：Base/Cell 双身体模型、结构化内存布局、生命周期管理。
>
> 来源：BigWorld 原创 Base/Cell 分离，KBEngine 完整继承。PropertyBlock 是 theseed 自研改进。

---

## 0.5 引擎实现对照与取舍

### BigWorld 是怎么实现的

```
BigWorld 的实体是 Base / Cell 双身体模型：
  - Base 负责长期状态与非实时逻辑
  - Cell 负责实时位置、AOI、战斗与视野
  - 两者通过 EntityCall 和复制协议协作
```

### KBEngine 是怎么实现的

```
KBEngine 基本继承了同样的 Base / Cell 模型：
  - BaseEntity / CellEntity 分工明确
  - Python 脚本承接业务逻辑
  - 结构上更贴近可直接落地的 runtime core
```

### 优缺点

```
共同优点：
  - 职责边界清楚
  - 实时和非实时逻辑自然拆分
  - 适合分布式部署

共同缺点：
  - 跨身体调用天然是网络边界
  - 状态同步和迁移复杂度高于单体实体
```

### theseed 的取舍

```
theseed 继续采用 Base / Cell 双身体模型，
但把属性布局、生命周期和复制边界写得更明确，
避免把脚本对象和运行时对象混成一层。
```

---

## 1. Entity 的两副身体：Base 和 Cell

```
一个逻辑实体（如"玩家"）在集群中有两副身体：

Base Entity                          Cell Entity
┌─────────────────────┐              ┌─────────────────────┐
│ 存在于 BaseApp       │              │ 存在于 CellApp       │
│ 生命周期长           │              │ 生命周期短            │
│ 持久化到数据库        │              │ 进入空间才创建        │
│ 处理非实时逻辑        │              │ 处理实时逻辑          │
│  - 邮件系统           │              │  - 位置/移动          │
│  - 好友系统           │              │  - 战斗/伤害          │
│  - 背包/装备          │              │  - AOI/视野           │
│  - 公会              │              │  - Ghost              │
│ 无空间位置            │              │ 有空间位置            │
└────────┬──────────────┘              └──────────┬──────────┘
         │                                        │
         └──── EntityCall 双向通信 ────────────────┘
```

---

## 2. Entity 内存布局

KBEngine 的 Entity 是 Python 对象，属性存储在 Python dict 中。theseed 改成结构化内存布局：

```cpp
// runtime/Entity.h

class Entity {
public:
    // --- 身份 ---
    EntityId id() const;
    const std::string& entityType() const;
    EntitySide side() const;               // Base / Cell

    // --- 生命周期 ---
    EntityState state() const;             // Creating / Active / Migrating / Destroying / Destroyed

    // --- Base ↔ Cell 连接 ---
    EntityCall* baseEntityCall() const;     // Cell → Base 的调用句柄
    EntityCall* cellEntityCall() const;     // Base → Cell 的调用句柄

    // --- 属性操作 ---
    template<typename T>
    T getProperty(PropertyId id) const;

    template<typename T>
    void setProperty(PropertyId id, const T& value);

    bool isPropertyDirty(PropertyId id) const;
    void clearDirtyFlags();

    // --- 空间（Cell 侧） ---
    SpaceId spaceId() const;
    const Vector3& position() const;
    const Vector3& direction() const;

    // --- Witness（Cell 侧，有客户端的实体） ---
    Witness* witness() const;
    bool isReal() const;                   // real vs ghost

    // --- 脚本对象 ---
    ScriptObject* scriptObject() const;

private:
    // --- 属性存储（结构化内存） ---
    // 不用 Python dict，用紧凑的内存块
    struct PropertyBlock {
        void* data;                        // 连续内存块
        const EntityDef* def;              // 属性定义（描述布局）
        DirtyMask dirtyMask;               // 脏标记位图
    };

    EntityId id_;
    uint16_t entityTypeUType_;
    EntitySide side_;
    std::atomic<EntityState> state_;

    // Base/Cell 连接
    EntityCall* baseCall_;
    EntityCall* cellCall_;

    // 属性
    PropertyBlock properties_;

    // Cell 侧
    CoordinateNode* coordNode_;
    Witness* witness_;
    SpaceId spaceId_;

    // Ghost 相关
    ComponentId realCell_;                 // 0=自己是real, 否则=real所在CellApp
    ComponentId ghostCell_;                // 0=无ghost, 否则=ghost所在CellApp
    GhostManager* ghostMgr_;

    // 脚本
    ScriptObject* scriptObj_;

    // 控制器
    ControllerStack controllers_;

    // 定时器
    std::vector<TimerHandle> timers_;
};
```

---

## 3. PropertyBlock：连续内存属性存储

KBEngine 把所有属性存在 Python dict 中，每次读写都要经过 Python API，性能差。

theseed 的改进：**属性用紧凑的内存块存储，脚本通过偏移量直接访问**。

```cpp
// runtime/PropertyBlock.h

// 编译期确定每个属性的偏移量和大小
// 运行时只需 pointer + offset 即可读写
class PropertyBlock {
public:
    // 初始化：根据 EntityDef 分配内存
    void init(const EntityDef& def);

    // 读写（零拷贝）
    template<typename T>
    const T& get(PropertyId id) const {
        auto& desc = def_->getProperty(id);
        return *reinterpret_cast<const T*>(
            data_ + desc.offset()
        );
    }

    template<typename T>
    void set(PropertyId id, const T& value) {
        auto& desc = def_->getProperty(id);
        *reinterpret_cast<T*>(data_ + desc.offset()) = value;
        dirtyMask_ |= (1ULL << id);
    }

    // 脏标记
    DirtyMask dirtyMask() const { return dirtyMask_; }
    void clearDirty() { dirtyMask_ = 0; }

    // 序列化（只序列化脏属性）
    void serializeDirty(MemoryStream& stream) const;
    void serializeAll(MemoryStream& stream) const;
    void deserialize(MemoryStream& stream);

    // 脚本层访问桥接
    // Python/Lua 通过 ScriptPropertyBridge 访问
    // 桥接层负责类型检查和转换

private:
    char* data_;                    // 连续内存块
    const EntityDef* def_;          // 属性定义（描述布局）
    DirtyMask dirtyMask_;           // 脏标记位图（uint64 或 vector<uint64>）
};
```

**与 KBEngine 的对比**：

```
KBEngine:
  entity.health = 75
    → Python _tp_setattro
    → Entity::onScriptSetAttribute
    → PropertyDescription::isSameType (类型检查)
    → PyDict_SetItem (存入 Python dict)
    → onDefDataChanged (通知变更)

theseed:
  entity.health = 75
    → ScriptPropertyBridge::set("health", 75)
    → PropertyBlock::set<INT32>(health_id, 75)
    → 直接写入内存偏移 + 设置脏标记位
    → 无 Python dict 开销
```

---

## 4. Entity 生命周期状态机

```
                    createEntity()
                         │
                         ▼
                   ┌───────────┐
                   │ Creating  │ def 加载 + 属性初始化 + 脚本对象创建
                   └─────┬─────┘
                         │ onInitialize()
                         ▼
                   ┌───────────┐
              ┌────│  Active   │──────────────────────┐
              │    └─────┬─────┘                      │
              │          │                            │
         teleport    destroy()                    migrate()
              │          │                            │
              ▼          ▼                            ▼
       ┌────────────┐ ┌────────────┐         ┌────────────┐
       │ Teleporting│ │ Destroying │         │ Migrating  │
       └─────┬──────┘ └─────┬──────┘         └─────┬──────┘
             │              │                       │
             ▼              ▼                       ▼
       Active(in new    ┌───────────┐         Active(in new
       Space/Cell)      │ Destroyed │         Process)
                       └───────────┘
```

---

## 5. Entity 创建流程

参考 KBEngine `Cellapp::onCreateCellEntityInNewSpaceFromBaseapp`：

```
1. 解析消息流：entityType / entityID / spaceID / hasClient
2. 查找或创建 SpaceMemory
3. 分配 Entity 内存
4. 调用 PropertyBlock::init(entityDef)
5. 调用 PropertyBlock::deserialize(stream)  // 从 Base 传来的初始数据
6. 建立 baseEntityCall_
7. 如果有客户端：
   a. 建立 clientEntityCall_
   b. 创建 Witness
   c. 调用 onGetWitness()
8. space->addEntity(entity)
9. entity->initializeEntity()  // 脚本层 onInitialize
10. space->addEntityToNode(entity)  // 加入 AOI 坐标系统
```

---

## 6. Runtime Memory

### 6.1 对象池

```
游戏服务器高频分配的对象：
  - MemoryStream（序列化缓冲区）
  - Bundle（网络消息包）
  - Entity（实体）
  - CoordinateNode（AOI 节点）

KBEngine 使用对象池（OBJECTPOOL_POINT）：
  MemoryStream::createPoolObject() / reclaimPoolObject()
  Witness::createPoolObject() / reclaimPoolObject()

theseed 继续使用对象池，但改进：
  - Arena 分配器（实体相关内存连续分配）
  - 水位监控（对象池水位上报到 Metrics）
  - 自动扩缩（根据使用模式调整池大小）
```

### 6.2 Bundle 生命周期

```
Bundle 是网络消息的载体，生命周期关键：

1. 获取 Bundle
   auto bundle = BundlePool::acquire();

2. 写入消息
   bundle->newMessage(msgId);
   (*bundle) << entityId;
   (*bundle) << propertyId;
   (*bundle) << value;

3. 发送
   channel->send(bundle);
   // 注意：发送后 bundle 的所有权转移给 Channel
   // Channel 发送完毕后自动归还到池

4. 接收
   auto bundle = channel->receive();
   // 处理完毕后
   BundlePool::release(bundle);
```
