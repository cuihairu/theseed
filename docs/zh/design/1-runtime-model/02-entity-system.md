# Entity System

> English | **中文**

> The core design of theseed entities: the Base/Cell dual-body model, structured memory layout, and lifecycle management.
>
> Origin: the BigWorld Base/Cell separation model.
> Reference implementation: KBEngine's lightweight BaseEntity / CellEntity approach.
> theseed adds the PropertyBlock structured memory layout.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
BigWorld's entities use the Base / Cell dual-body model:
  - Base owns long-term state and non-realtime logic
  - Cell owns realtime position, AOI, combat, and view
  - The two collaborate through EntityCall and the replication protocol
```

### How KBEngine Implements It

```
KBEngine provides a lighter-weight Base / Cell approach:
  - BaseEntity / CellEntity have clearly separated responsibilities
  - Python scripts carry the business logic
  - Structurally closer to a directly implementable runtime core
```

### Pros and Cons

```
Shared pros:
  - Clear responsibility boundaries
  - Realtime and non-realtime logic split naturally
  - Suited to distributed deployment

Shared cons:
  - Cross-body calls are inherently network boundaries
  - State synchronization and migration are more complex than with a monolithic entity
```

### theseed's Trade-offs

```
theseed continues to use the Base / Cell dual-body model,
but writes the property layout, lifecycle, and replication boundaries more explicitly,
avoiding mixing script objects and runtime objects into a single layer.
```

---

## 1. Entity's Two Bodies: Base and Cell

```
A logical entity (such as a "player") has two bodies in the cluster:

Base Entity                          Cell Entity
┌───────────────────────┐              ┌───────────────────────┐
│ Lives in BaseApp      │              │ Lives in CellApp      │
│ Long lifecycle        │              │ Short lifecycle       │
│ Persisted to database │              │ Created on space entry│
│ Non-realtime logic    │              │ Realtime logic        │
│  - Mail system        │              │  - Position/movement  │
│  - Friends system     │              │  - Combat/damage      │
│  - Inventory/equipment│              │  - AOI/view           │
│  - Guild              │              │  - Ghost              │
│ No position           │              │ Has position          │
└────────┬──────────────┘              └──────────┬────────────┘
         │                                        │
         └── EntityCall two-way communication ──┘
```

---

## 2. Entity Memory Layout

KBEngine's Entity is a Python object, with properties stored in a Python dict. theseed switches to a structured memory layout:

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

## 3. PropertyBlock: Contiguous Memory Property Storage

KBEngine stores all properties in a Python dict, so every read and write goes through the Python API, resulting in poor performance.

theseed's improvement: **properties are stored in a compact memory block, and scripts access them directly by offset**.

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

**Comparison with KBEngine**:

```
KBEngine:
  entity.health = 75
    → Python _tp_setattro
    → Entity::onScriptSetAttribute
    → PropertyDescription::isSameType (type check)
    → PyDict_SetItem (store into the Python dict)
    → onDefDataChanged (notify of the change)

theseed:
  entity.health = 75
    → ScriptPropertyBridge::set("health", 75)
    → PropertyBlock::set<INT32>(health_id, 75)
    → write directly to the memory offset + set the dirty flag bit
    → no Python dict overhead
```

---

## 4. Entity Lifecycle State Machine

```
                    createEntity()
                         │
                         ▼
                   ┌───────────┐
                   │ Creating  │ def loading + property initialization + script object creation
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
       Active (in new    ┌───────────┐         Active (in new
       Space/Cell)      │ Destroyed │         Process)
                       └───────────┘
```

---

## 5. Entity Creation Flow

Reference: KBEngine `Cellapp::onCreateCellEntityInNewSpaceFromBaseapp`:

```
1. Parse the message stream: entityType / entityID / spaceID / hasClient
2. Look up or create SpaceMemory
3. Allocate Entity memory
4. Call PropertyBlock::init(entityDef)
5. Call PropertyBlock::deserialize(stream)  // initial data passed in from Base
6. Establish baseEntityCall_
7. If there is a client:
   a. Establish clientEntityCall_
   b. Create the Witness
   c. Call onGetWitness()
8. space->addEntity(entity)
9. entity->initializeEntity()  // script-layer onInitialize
10. space->addEntityToNode(entity)  // add to the AOI coordinate system
```

---

## 6. Runtime Memory

### 6.1 Object Pool

```
Objects allocated at high frequency on a game server:
  - MemoryStream (serialization buffer)
  - Bundle (network message packet)
  - Entity (entity)
  - CoordinateNode (AOI node)

KBEngine uses an object pool (OBJECTPOOL_POINT):
  MemoryStream::createPoolObject() / reclaimPoolObject()
  Witness::createPoolObject() / reclaimPoolObject()

theseed continues to use object pools, but with improvements:
  - Arena allocator (contiguous allocation of entity-related memory)
  - Watermark monitoring (object pool watermarks reported to Metrics)
  - Auto scaling (adjust pool size according to usage patterns)
```

### 6.2 Bundle Lifecycle

```
A Bundle is the carrier of network messages, and its lifecycle is critical:

1. Acquire a Bundle
   auto bundle = BundlePool::acquire();

2. Write messages
   bundle->newMessage(msgId);
   (*bundle) << entityId;
   (*bundle) << propertyId;
   (*bundle) << value;

3. Send
   channel->send(bundle);
   // Note: after sending, ownership of the bundle transfers to the Channel
   // The Channel returns it to the pool automatically once sending completes

4. Receive
   auto bundle = channel->receive();
   // After handling is complete
   BundlePool::release(bundle);
```
