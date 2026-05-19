# Runtime Core Design — 引擎运行时核心

> thised 最底层的设计文档。所有其他设计（可观测、基础设施、SDK）都构建在这之上。
> 如果这篇文档不够扎实，其他都是空中楼阁。
>
> 核心来源：KBEngine + BigWorld 的实际实现，不是凭空想象。

---

## 1. 核心问题：我们要实现什么

```
一个实体存在于服务器集群中，它需要：

1. 在正确的进程上运行（Base 或 Cell）
2. 在正确的空间中有位置（Cell 侧）
3. 被正确的客户端看到（Witness）
4. 被跨边界的邻居感知到（Ghost）
5. 属性变更能高效同步给所有相关方
6. 可以安全地跨进程迁移
7. 状态能持久化和恢复
```

这 7 件事决定了 Runtime Core 的所有组件。

---

## 2. Tick Model

### 2.1 为什么是单线程 tick

KBEngine 和 BigWorld 都是 **单线程 tick 模型**。这不是落后，而是游戏逻辑的正确抽象：

```
原因 1：确定性
  - 游戏逻辑必须可预测：同一输入 → 同一输出
  - 多线程并发写同一实体会打破这个保证
  - 锁的开销在高频 tick（10-20Hz）下不可接受

原因 2：简单性
  - 脚本层不需要关心线程安全
  - Python GIL 本身就是单线程
  - 开发者心智负担最低

原因 3：性能
  - 单线程无锁，cache 友好
  - 游戏逻辑通常不是 CPU 瓶颈（网络 I/O 和 AOI 才是）
  - 真正的并行靠分布式（多进程），不是多线程
```

### 2.2 Tick 生命周期

```
┌─────────────────────────────────────────────────────────┐
│  一个 Tick (典型 10Hz → 100ms 预算)                       │
│                                                          │
│  1. Network Process                                      │
│     ├─ 接收所有就绪消息                                    │
│     ├─ 反序列化到 Message 队列                              │
│     └─ 按 Channel 顺序分发                                 │
│                                                          │
│  2. Timer Callbacks                                      │
│     ├─ 到期的定时器触发回调                                  │
│     └─ 按注册顺序执行                                      │
│                                                          │
│  3. Entity Processing (CellApp only)                     │
│     ├─ AOI 更新（坐标节点移动 → 触发器事件）                  │
│     ├─ Witness 更新（视野进出 + 属性同步）                   │
│     ├─ 移动控制器 tick                                     │
│     └─ Ghost 同步（real → ghost 属性推送）                  │
│                                                          │
│  4. Script Execution                                     │
│     ├─ 消息触发的脚本回调                                    │
│     ├─ 定时器触发的脚本回调                                  │
│     └─ 所有脚本回调在主线程同步执行                           │
│                                                          │
│  5. Sync & Flush                                         │
│     ├─ 收集所有脏属性                                      │
│     ├─ 构造同步 Bundle                                     │
│     ├─ 发送给 Ghost                                        │
│     ├─ 发送给 Witness（客户端）                              │
│     └─ flush 所有 Channel                                  │
│                                                          │
│  6. Idle                                                 │
│     └─ 等待下一个 tick 唤醒                                 │
└─────────────────────────────────────────────────────────┘
```

### 2.3 核心接口

```cpp
// runtime/TickModel.h

class ITickable {
public:
    virtual ~ITickable() = default;
    virtual void tick(Duration deltaTime) = 0;
};

class TickScheduler {
public:
    // 注册 tick 对象
    void registerTickable(ITickable* obj, TickPhase phase);

    // 主循环
    void run();
    void stop();

    // 当前 tick 信息
    uint64_t currentTick() const;
    Duration tickBudget() const;        // 配置的 tick 预算
    Duration lastTickDuration() const;  // 上次 tick 实际耗时

private:
    // tick 阶段（按顺序执行）
    enum class TickPhase {
        Network,        // 网络消息处理
        Timer,          // 定时器回调
        Entity,         // 实体处理（AOI/Witness/Movement）
        Script,         // 脚本执行
        Sync,           // 同步 & flush
    };

    std::array<std::vector<ITickable*>, 5> phaseQueues_;
};
```

### 2.4 与 KBEngine 的对应

```
KBEngine:
  EventDispatcher::processUntilBreak()
    → processNetworkRequests()     → Phase::Network
    → processTimers()              → Phase::Timer
    → Cellapp::processEntityTick() → Phase::Entity
    → script execute               → Phase::Script
    → Witness::update()            → Phase::Sync

theseed:
  TickScheduler::run()
    → tick(Network)
    → tick(Timer)
    → tick(Entity)
    → tick(Script)
    → tick(Sync)
```

---

## 3. Entity Runtime

### 3.1 Entity 的两副身体：Base 和 Cell

这是 BigWorld/KBEngine 最核心的设计，theseed 继承并改进：

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

### 3.2 Entity 内存布局

KBEngine 的 Entity 是 Python 对象，属性存储在 Python dict 中。这些ed 要改成结构化内存布局：

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

### 3.3 PropertyBlock：连续内存属性存储

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

### 3.4 Entity 生命周期状态机

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

### 3.5 Entity 创建流程

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

## 4. AOI 系统

### 4.1 设计选择：十字链表

KBEngine 和 BigWorld 都使用**十字链表（Coordinate System / Range List）**作为 AOI 索引。

为什么不用网格或四叉树/八叉树：

```
十字链表优势：
  1. 实体移动时只需 O(1) 调整链表位置
  2. 范围查询：在每条轴上找范围 → 取交集
  3. 触发器可以"挂"在链表节点上，节点经过时自动触发
  4. 不需要预分配网格，实体密度不均匀时也高效

劣势：
  1. 范围查询不是 O(1)，是 O(视野内实体数)
  2. 不适合超大范围查询（如全地图广播）

结论：MMO 中 AOI 通常在 50-500 米范围，十字链表是最优选择。
```

### 4.2 核心组件

```
AOI 系统组件关系（来自 KBEngine 源码分析）：

CoordinateSystem
  ├── CoordinateNode（每个实体一个）
  │     ├── x_prev / x_next  ← X 轴双向链表
  │     ├── y_prev / y_next  ← Y 轴双向链表（可选）
  │     └── z_prev / z_next  ← Z 轴双向链表
  │
  ├── RangeTrigger（每个有视野的实体一个或多个）
  │     ├── 在每条轴上安装边界节点
  │     ├── 节点穿越边界时触发回调
  │     └── X/Y/Z 三轴取交集确定进入/离开
  │
  └── Witness（每个有客户端的实体一个）
        ├── ViewTrigger：主视野半径
        ├── HysteresisTrigger：滞后防抖区域
        ├── viewEntities_：当前可见实体集合
        ├── changeDefDataLogs[]：按 detailLevel 分组的脏属性
        └── update()：tick 末执行，处理进出+同步
```

### 4.3 核心接口

```cpp
// runtime/CoordinateSystem.h

class CoordinateSystem {
public:
    // 插入实体节点
    void insert(CoordinateNode* node);

    // 移除实体节点
    void remove(CoordinateNode* node);

    // 移动节点（实体位置变化时调用）
    void update(CoordinateNode* node, const Vector3& newPos);

    // 范围查询
    void entitiesInRange(std::vector<Entity*>& out,
                         const Vector3& origin,
                         float radius,
                         uint16_t entityTypeFilter = 0) const;
};

class CoordinateNode {
public:
    Entity* entity() const;
    const Vector3& position() const;

    // 链表操作（内部使用）
    void insertX(CoordinateNode* pos);
    void insertY(CoordinateNode* pos);
    void insertZ(CoordinateNode* pos);
    void removeX();
    void removeY();
    void removeZ();

private:
    Entity* entity_;
    Vector3 pos_;

    // 三轴双向链表指针
    CoordinateNode* xPrev_; CoordinateNode* xNext_;
    CoordinateNode* yPrev_; CoordinateNode* yNext_;
    CoordinateNode* zPrev_; CoordinateNode* zNext_;
};

// runtime/RangeTrigger.h

class RangeTrigger {
public:
    RangeTrigger(Entity* owner, float range);
    ~RangeTrigger();

    // 在坐标系统中安装/卸载
    void install(CoordinateSystem* coordSys);
    void uninstall();

    // 更新范围
    void updateRange(float newRange);

    // 节点穿越边界时的回调
    virtual void onEnter(CoordinateNode* node) = 0;
    virtual void onLeave(CoordinateNode* node) = 0;

private:
    Entity* owner_;
    float range_;
    CoordinateSystem* coordSys_;

    // 边界节点（每条轴的正负方向各一个）
    CoordinateNode* xNegBound_; CoordinateNode* xPosBound_;
    CoordinateNode* yNegBound_; CoordinateNode* yPosBound_;
    CoordinateNode* zNegBound_; CoordinateNode* zPosBound_;
};
```

### 4.4 Witness：视野系统

```cpp
// runtime/Witness.h

class Witness : public ITickable {
public:
    // 附加/分离
    void attach(Entity* entity);
    void detach();

    // 配置视野
    void setViewRadius(float radius, float hysteresis);

    // AOI 回调
    void onEnterView(RangeTrigger* trigger, Entity* entity);
    void onLeaveView(RangeTrigger* trigger, Entity* entity);

    // tick 末执行
    void tick(Duration dt) override;

    // 查询
    const std::vector<EntityRef>& viewEntities() const;
    bool entityInView(EntityId id) const;

    // 发送给客户端
    bool sendToClient(const MessageHandler& handler, Bundle& bundle);

private:
    Entity* owner_;

    // 两层触发器
    ViewTrigger* viewTrigger_;              // 主视野
    ViewTrigger* hysteresisTrigger_;        // 滞后防抖

    // 可见实体集合
    std::vector<EntityRef> viewEntities_;
    std::unordered_map<EntityId, size_t> viewEntityMap_;

    // 脏属性记录（按 detailLevel 分组）
    struct WitnessInfo {
        int8_t detailLevel;                 // 0=近 1=中 2=远
        Entity* entity;
        float range;
        bool detailLevelLog[3];
        std::vector<PropertyId> changedProps[3];
    };
    std::unordered_map<EntityId, WitnessInfo> witnessInfo_;

    float viewRadius_;
    float hysteresisArea_;
};
```

### 4.5 Witness::tick() 的核心逻辑

```
Witness::tick() — 每 tick 末执行：

1. 处理视野进出事件
   onEnterView → 发送 createEntity 消息给客户端
                 → 记录到 viewEntities_
   onLeaveView → 发送 leaveEntity 消息给客户端
                 → 从 viewEntities_ 移除

2. 更新 detailLevel
   对每个 viewEntity：
     计算 distance
     distance < near_threshold → detailLevel = 0
     distance < far_threshold  → detailLevel = 1
     else                      → detailLevel = 2
   如果 detailLevel 变化了：
     从低级别升到高级别 → 发送新级别的属性
     从高级别降到低级别 → 不需要发送（客户端自己清理）

3. 广播脏属性
   对每个 viewEntity：
     检查 changedProps[detailLevel]
     有脏属性 → 序列化到 Bundle → 发送给客户端

4. 处理位置/朝向更新（Volatile）
   对每个 viewEntity：
     位置变化 > threshold → 发送位置更新
     朝向变化 > threshold → 发送朝向更新

5. flush Bundle
   将所有积累的消息通过 Channel 发送给客户端
```

---

## 5. Ghost 系统

### 5.1 为什么需要 Ghost

```
问题：
  Space A 在 CellApp 1，Space B 在 CellApp 2
  实体 X 在 Space A，但 X 的 AOI 覆盖到了 Space B
  Space B 上的玩家看到 X 应该出现在视野中

  没有 Ghost：
    Space B 不知道 X 存在 → AOI 事件无法触发 → 玩家看不到 X

  有 Ghost：
    在 Space B 上创建 X 的只读副本（Ghost X）
    Ghost X 参与 Space B 的 AOI → 触发器正常工作
    玩家通过 Ghost X 看到 X 的状态
```

### 5.2 Real / Ghost 权威模型

```
核心原则：单一写权限

  Real Entity:
    - 拥有完整属性
    - 拥有写权限
    - 处理所有逻辑
    - 位置：real 所在的 CellApp

  Ghost Entity:
    - 拥有部分属性（只有同步过来的）
    - 只读
    - 不处理逻辑
    - 方法调用转接到 Real
    - 位置：AOI 覆盖到的其他 CellApp

  Real → Ghost 同步：属性变更时 real 主动推送给 ghost
  Ghost → Real：方法调用通过 RealEntityMethod 转接
```

### 5.3 核心接口

```cpp
// runtime/GhostManager.h

class GhostManager {
public:
    // Real 侧操作
    void createGhost(ComponentId targetCellApp);     // 在远端创建 ghost
    void destroyGhost(ComponentId targetCellApp);    // 销毁远端 ghost
    void syncToGhost(Bundle& bundle);               // 推送属性变更到 ghost

    // Ghost 侧操作
    void onGhostSync(Bundle& bundle);               // 接收 real 的属性同步
    void forwardToReal(const std::string& method,   // 将方法调用转发到 real
                       Bundle& args);

    // 状态查询
    bool isReal() const { return realCell_ == 0; }
    bool hasGhost() const { return ghostCell_ > 0; }

    // 迁移路由（KBEngine 的临时路由机制）
    void setRoute(ComponentId target, Duration ttl);
    void clearRoute();
    ComponentId getRoute() const;

private:
    Entity* owner_;
    ComponentId realCell_;       // 0=自己是 real
    ComponentId ghostCell_;      // 0=无 ghost
    ComponentId routeTarget_;    // 迁移窗口的临时路由目标
    TimePoint routeExpiry_;      // 路由过期时间
};
```

### 5.4 Ghost 消息转发

```
Ghost 上的方法调用流程（来自 KBEngine RealEntityMethod）：

1. 脚本在 Ghost 上调用 methodA(args)
2. GhostManager 发现自己是 ghost（isReal() == false）
3. 构造转发 Bundle：
   - 目标：realCell_（real 所在的 CellApp）
   - 消息：onGhostMessage
   - 内容：entityID + methodName + args
4. 发送到 real 所在 CellApp
5. Real 收到后执行真正的 methodA(args)
6. 如果方法产生属性变更 → 正常走 onDefDataChanged
   → 触发 real → ghost 同步
```

---

## 6. Replication（属性复制与同步）

### 6.1 同步方向

```
属性同步有四条路径：

Path 1: Real → Ghost（CellApp 内部同步）
  触发：属性变更 onDefDataChanged
  条件：isReal() && hasGhost() && 属性标记 CELL_PUBLIC
  方式：立即发 Bundle 给 ghost CellApp

Path 2: Real → Witness（服务端到客户端）
  触发：属性变更 onDefDataChanged
  条件：属性标记 OTHER_CLIENTS 或 OWN_CLIENT
  方式：
    OWN_CLIENT: 直接发给自己客户端
    OTHER_CLIENTS: 遍历 witnesses 发送
    注意：非 volatile 属性是立即发送，不是 tick 末批量

Path 3: Witness → Client（tick 末批量）
  触发：Witness::tick()
  内容：
    - 视野进出事件
    - 位置/朝向 volatile 更新
    - detailLevel 变更
    - 脏属性按 detailLevel 过滤后发送
  方式：一个 Bundle 包含所有更新，一次发送

Path 4: Entity → Database（持久化）
  触发：setDirty() 标记 + 定时 save
  条件：属性标记 Persistent
  方式：定时（如 30s）将所有脏属性序列化写库
```

### 6.2 脏标记系统

```cpp
// runtime/DirtyMask.h

// 使用位图标记哪些属性变更了
// uint64 可覆盖 64 个属性，超过则用 vector<uint64>

class DirtyMask {
public:
    void mark(PropertyId id) {
        mask_ |= (1ULL << id);
    }

    bool isDirty(PropertyId id) const {
        return (mask_ & (1ULL << id)) != 0;
    }

    bool any() const { return mask_ != 0; }

    void clear() { mask_ = 0; }

    // 遍历所有脏属性
    template<typename Func>
    void foreachDirty(Func&& fn) const {
        uint64_t m = mask_;
        PropertyId id = 0;
        while (m) {
            if (m & 1) fn(id);
            m >>= 1;
            id++;
        }
    }

private:
    uint64_t mask_ = 0;
};
```

### 6.3 同步 Bundle 构造

```
tick 末 Witness::tick() 构造同步 Bundle 的逻辑：

Bundle layout:
┌─────────────────────────────────────────────────────────┐
│ Header: 消息 ID (aliasID 或 utype)                       │
├─────────────────────────────────────────────────────────┤
│ Entity 1:                                               │
│   entityID (aliasID 如果可用)                             │
│   detailLevel 变更标志                                    │
│   ┌─ 脏属性 bitmap (只包含当前 detailLevel 的属性)         │
│   │  属性值序列化...                                       │
│   └─                                                     │
│   volatile 数据 (位置/朝向)                                │
├─────────────────────────────────────────────────────────┤
│ Entity 2: ...                                           │
├─────────────────────────────────────────────────────────┤
│ Tail: 结束标记                                           │
└─────────────────────────────────────────────────────────┘

带宽优化手段（来自 KBEngine）：
  1. aliasID: 属性数 < 255 时用 1 字节代替 2 字节 utype
  2. detailLevel: 远处实体只同步部分属性
  3. 脏属性 bitmap: 只发送变化的属性
  4. volatile threshold: 位置/朝向变化超过阈值才同步
  5. entity alias: 首次发送 entity type string，后续用 1 字节 alias
```

---

## 7. Entity Ownership 与 EntityCall

### 7.1 EntityCall 是什么

```
EntityCall 不是"远程方法调用框架"。
它是"实体在另一个进程上的代理句柄"。

entity.base.onLevelUp(35)
  │       │
  │       └── base 是一个 EntityCall，指向 Base 侧实体
  │
  └── 在 Cell 侧执行，通过 EntityCall 把调用发到 BaseApp

本质：
  EntityCall = (EntityID, ComponentID, EntityType)
  调用 EntityCall 的方法 = 发送一条消息到目标进程

关键区别：
  EntityCall 是有状态的——它绑定到特定实体
  gRPC 是无状态的——它只绑定到服务端点
```

### 7.2 核心设计决策：EntityCall 必须走 Runtime Transport

```
重要：EntityCall 不走 MessageBus！

原因：
  1. 顺序保证：同一 Channel 上的消息必须保序
     MessageBus（NATS）不保证消息顺序
     Runtime Transport（TCP 直连）保证 FIFO

  2. 延迟：EntityCall 是 tick 内同步操作
     NATS 中间层增加至少 1-2ms 延迟
     TCP 直连是 sub-ms

  3. 所有权敏感：EntityCall 涉及实体权威
     消息路由需要知道实体当前在哪个进程
     这个路由信息在 Runtime 内部维护，不在 MQ 中

  4. 背压：tick 内不能无限发消息
     Runtime Transport 有 Channel 水位控制
     MQ 通常不做 tick 级背压

结论：
  EntityCall → Runtime Transport（进程间 TCP 直连）
  控制面消息 → MessageBus（NATS）
  异步任务 → MessageBus
  跨服桥接 → MessageBus
```

### 7.3 EntityCall 接口

```cpp
// runtime/EntityCall.h

class EntityCall {
public:
    EntityCall(EntityId id, ComponentId target, const std::string& entityType);

    // 调用远端方法（fire-and-forget）
    void call(const std::string& method, const Args& args);

    // 调用远端方法（带回调）
    void callWithCallback(const std::string& method,
                          const Args& args,
                          CallbackPtr callback);

    // 底层：构造 Bundle 并发送
    void sendCall(Bundle* bundle);

    // 路由：决定消息发到哪个进程
    ComponentId targetComponent() const;
    void updateTarget(ComponentId newTarget);  // 迁移后更新路由

    // 状态
    bool isValid() const;
    const Address& address() const;

private:
    EntityId entityId_;
    ComponentId targetComponent_;
    uint16_t entityTypeUType_;
    Address targetAddr_;
    Channel* channel_;
};
```

### 7.4 Base ↔ Cell 通信

```
通信模式：

Base → Cell:
  Base 持有 cellEntityCall
  通过 cellEntityCall.call() 发消息到 CellApp

Cell → Base:
  Cell 持有 baseEntityCall
  通过 baseEntityCall.call() 发消息到 BaseApp

创建流程：
  1. Base 创建实体 → DB 写入
  2. Base 通过 EntityCall 请求 CellApp 创建 Cell Entity
  3. CellApp 创建成功后，Cell Entity 持有 baseEntityCall
  4. 双向通信链路建立

销毁流程：
  1. Cell 调用 destroy() → 通知 Base onLoseCell
  2. Base 决定是否销毁 Base Entity
  3. 如果是，Base 销毁 → 通知 DB 删除
  4. 如果不是（如玩家断线但保留数据），Base 保持存活
```

---

## 8. Entity Migration（实体迁移）

### 8.1 迁移场景

```
场景 1: Teleport（跨 Space 传送）
  玩家从 Space A 传送到 Space B
  可能跨 CellApp

场景 2: Cell 边界移动
  Space 拓扑变化导致 Cell 边界移动
  实体需要迁移到新的 CellApp

场景 3: 负载均衡
  CellApp 过载，部分实体迁移到其他 CellApp

场景 4: 跨服
  实体从一个 Realm 迁移到另一个 Realm
```

### 8.2 迁移流程

```
Phase 1: Prepare
  ├─ 源 CellApp: 冻结实体（不接受新消息）
  ├─ 设置 GhostManager 路由（转发窗口期的消息）
  └─ 通知目标 CellApp 准备接收

Phase 2: Serialize
  ├─ 序列化实体状态：
  │   - 所有持久化属性
  │   - 所有 volatile 属性
  │   - 定时器列表
  │   - 控制器状态
  │   - Witness 状态
  │   - 脚本栈快照（如果支持）
  └─ 发送到目标 CellApp

Phase 3: Restore
  ├─ 目标 CellApp: 创建新实体
  ├─ 反序列化属性
  ├─ 恢复定时器
  ├─ 加入 Space / CoordinateSystem
  └─ 恢复 Witness（如果有客户端）

Phase 4: Route Update
  ├─ 更新 EntityCall 路由：旧 CellApp → 新 CellApp
  ├─ 通知 BaseApp 更新 cellEntityCall 目标
  ├─ GhostManager 路由窗口期：将缓存消息转发到新位置
  └─ 通知所有持有 EntityCall 的进程更新地址

Phase 5: Cleanup
  ├─ 旧 CellApp: 销毁旧实体（callScript=false）
  ├─ 清理 GhostManager 路由
  └─ 解冻实体
```

### 8.3 迁移窗口期的消息处理

```
问题：迁移过程中，消息可能发到旧的 CellApp

KBEngine 的解决方案（来自源码注释）：
  "如果期间有base的消息发送过来，entity的ghost机制能够转到real上去"

theseed 的设计：
  GhostManager.setRoute(target, ttl=10s)
  → 旧 CellApp 在路由窗口期内，把所有发给该实体的消息转发到新位置
  → 路由过期后，如果还有消息发来，说明路由表没更新完，记录告警
  → 这个机制保证了迁移期间消息零丢失
```

---

## 9. Timer & Scheduler

### 9.1 定时器模型

```
游戏服务器的定时器需求：
  1. 高频（每秒数十万次触发）
  2. 精度要求不高（10ms 级别即可）
  3. 主要用于 tick 驱动的回调
  4. 支持一次性 / 周期性
  5. 支持取消

经典方案：时间轮（Hashed Timing Wheel）
  - O(1) 添加/取消
  - 精度取决于 wheel 的 tick duration
  - 适合大量定时器场景
```

### 9.2 核心接口

```cpp
// runtime/TimerWheel.h

class TimerWheel {
public:
    // 创建定时器
    TimerHandle addTimer(Duration interval,
                         TimerCallback callback,
                         bool periodic = true);

    // 取消定时器
    void cancelTimer(TimerHandle handle);

    // tick 推进
    void advance(Duration dt);

    // 统计
    size_t activeTimerCount() const;

private:
    static constexpr int WHEEL_BITS = 8;
    static constexpr int WHEEL_SIZE = 1 << WHEEL_BITS;  // 256
    static constexpr Duration TICK_DURATION = 10ms;

    struct TimerEntry {
        Duration deadline;
        Duration interval;
        TimerCallback callback;
        bool periodic;
        bool cancelled;
    };

    std::array<std::vector<TimerEntry>, WHEEL_SIZE> wheels_[4]; // 4 级时间轮
    std::unordered_map<TimerId, TimerEntry> timers_;
};
```

---

## 10. Runtime Memory

### 10.1 对象池

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

### 10.2 Bundle 生命周期

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

---

## 11. 与前序设计文档的关系

```
00-runtime-core.md  ← 你正在读的，地基
  │
  ├── 01-developer-experience.md
  │     Debug/Profile/Ops 都是在 Runtime Core 的接口上挂 probe
  │
  ├── 02-observability.md
  │     OTel 的 Tracing 通过 EntityCall context propagation 实现
  │     Metrics 采集 Runtime 的 tick/entity/aoi/db 指标
  │
  ├── 03-infrastructure.md
  │     MessageBus 只做控制面和跨服，不做 EntityCall
  │     Gateway 不参与 Runtime 内部通信
  │     Redis 做缓存，不做实体存储
  │
  ├── 04-client-sdk.md
  │     SDK 的属性同步模型来自 Witness::tick() 的逻辑
  │     detailLevel / aliasID / volatile 在这篇有完整定义
  │
  └── 05-script-security.md
        沙箱限制的对象就是 ScriptObject（Entity 上的脚本代理）
        安全检查在 PropertyBlock::set() 和 EntityCall::call() 中执行
```

---

## 12. 目录结构

```
theseed/
├── src/
│   ├── runtime/                     # Runtime Core（本文档覆盖）
│   │   ├── CMakeLists.txt
│   │   ├── Entity.h/cpp             # Entity 核心
│   │   ├── PropertyBlock.h/cpp      # 属性存储
│   │   ├── DirtyMask.h              # 脏标记
│   │   ├── EntityCall.h/cpp         # 实体间调用
│   │   ├── EntityState.h            # 实体状态机
│   │   ├── TickScheduler.h/cpp      # Tick 调度器
│   │   ├── TimerWheel.h/cpp         # 定时器
│   │   │
│   │   ├── aoi/                     # AOI 子系统
│   │   │   ├── CoordinateSystem.h/cpp
│   │   │   ├── CoordinateNode.h/cpp
│   │   │   ├── RangeTrigger.h/cpp
│   │   │   └── Witness.h/cpp
│   │   │
│   │   ├── ghost/                   # Ghost 子系统
│   │   │   ├── GhostManager.h/cpp
│   │   │   └── RealEntityMethod.h/cpp
│   │   │
│   │   ├── sync/                    # 属性同步
│   │   │   ├── PropertySync.h/cpp   # 同步引擎
│   │   │   ├── BundleBuilder.h/cpp  # Bundle 构造
│   │   │   └── VolatileInfo.h/cpp   # Volatile 属性管理
│   │   │
│   │   ├── migration/               # 实体迁移
│   │   │   ├── MigrationManager.h/cpp
│   │   │   └── EntityStateSerializer.h/cpp
│   │   │
│   │   └── memory/                  # 内存管理
│   │       ├── ObjectPool.h         # 通用对象池
│   │       ├── BundlePool.h/cpp     # Bundle 对象池
│   │       └── ArenaAllocator.h     # Arena 分配器
│   │
│   └── ...
│
└── docs/
    └── design/
        └── 00-runtime-core.md       # 本文档
```

---

## 13. 实现优先级

```
必须最先实现的（没有这些，引擎跑不起来）：

P0 - 地基
  1. TickScheduler（主循环）
  2. Entity + PropertyBlock（实体核心）
  3. EntityCall（进程间通信）
  4. CoordinateSystem + CoordinateNode（AOI 索引）
  5. RangeTrigger（AOI 触发器）
  6. Witness（视野系统）
  7. PropertySync + DirtyMask（属性同步）
  8. TimerWheel（定时器）

P1 - 核心
  9. GhostManager（Ghost 系统）
  10. MigrationManager（实体迁移）
  11. VolatileInfo（位置/朝向同步优化）
  12. ObjectPool + BundlePool（内存管理）

P2 - 改进
  13. detailLevel 优化（远距离降级同步）
  14. aliasID 优化（带宽节省）
  15. ArenaAllocator（内存连续性优化）
```

这些才是引擎的骨头。可观测、基础设施、SDK 都是长在骨头上的肉。

---

## 14. Transport Reliability — 消息可靠性分级

> 来源：BigWorld Mercury UDP 四级可靠性。
> KBEngine 选 TCP，简单但一刀切——所有消息要么全可靠，要么全不可靠。
> theseed 取两者之长：**同一连接上支持可靠性分级**。

### 14.1 为什么需要分级

```
游戏服务器同一条连接上的消息，可靠性需求截然不同：

必须可靠（丢了就崩）：
  - EntityCall 方法调用
  - 实体创建 / 销毁
  - 属性持久化写入
  - 迁移序列化数据

丢了无所谓（下帧覆盖）：
  - 位置更新（每 tick 都有新位置）
  - 朝向更新
  - 速度同步

尽量可靠但不致命：
  - 属性同步（可以容忍偶尔丢一帧，下帧补上）
  - AOI 进入/离开通知（丢了会在下个 tick 重算）
```

### 14.2 四级可靠性（来自 BigWorld Mercury）

```
这些ed 的可靠性分级：

enum class Reliability : uint8_t {
    NONE        = 0,   // 不保证送达，不重传（位置/朝向）
    LOW         = 1,   // 尽力送达，丢了不重传（属性同步）
    HIGH        = 2,   // 保证送达，重传直到 ACK（EntityCall）
    CRITICAL    = 3,   // 保证送达 + 保证顺序（实体创建/迁移/销毁）
};
```

### 14.3 theseed 的实现策略

```
BigWorld 的做法：自己用 UDP + 自建可靠性层（Mercury）
  优势：极致性能，同一通道混用可靠/不可靠
  代价：实现复杂度极高（序列号/ACK/窗口/重传/分片/Piggyback）

KBEngine 的做法：全部走 TCP
  优势：实现简单，操作系统保证可靠性
  代价：位置更新也走 TCP 可靠传输，浪费带宽和延迟

theseed 的折中：
  进程间内部通信：
    方案 A（推荐）：KCP over UDP
      - KCP 提供 Mercury 类似的能力：可靠/不可靠混用
      - 比 Mercury 实现简单得多（开源成熟库）
      - 延迟接近原生 UDP（比 TCP 低 30-40%）
      - 支持 KCP 的各种模式（快速/普通/低速）

    方案 B（简单部署）：TCP + 可靠性标记
      - 仍然走 TCP
      - 可靠性标记作为元数据保留
      - 位置更新走独立 UDP 端口（不可靠通道）
      - 实现简单，但不如方案 A 干净

  客户端通信：
    TCP（可靠）+ 可选 UDP（不可靠位置更新）
    或 WebSocket（Web 客户端）
```

### 14.4 核心接口

```cpp
// runtime/ReliableTransport.h

class IReliableTransport {
public:
    virtual ~IReliableTransport() = default;

    // 发送消息，指定可靠性级别
    virtual void send(const Address& target,
                      const Message& msg,
                      Reliability reliability) = 0;

    // 注册消息处理器
    virtual void registerHandler(MessageId id,
                                 MessageHandler handler) = 0;

    // Channel 管理
    virtual Channel* getChannel(const Address& addr) = 0;
    virtual void closeChannel(const Address& addr) = 0;

    // tick 驱动（处理 ACK/重传/发送队列）
    virtual void tick() = 0;

    // 统计
    virtual TransportStats getStats() const = 0;
};
```

### 14.5 消息头中的可靠性标记

```
Bundle 消息头格式（theseed）：

┌─────────────────────────────────────────┐
│ Message ID (2 bytes)                    │
│ Reliability (2 bits)                    │  ← 新增
│ Compressed (1 bit)                      │
│ Reserved (5 bits)                       │
│ Payload Length (2 bytes)                │
├─────────────────────────────────────────┤
│ Payload ...                             │
└─────────────────────────────────────────┘

发送端：
  1. 根据 MessageDescription 的可靠性配置设置标记
  2. CRITICAL/HIGH 消息进入可靠发送队列（等待 ACK）
  3. LOW/NONE 消息直接发送（不等待）

接收端：
  1. CRITICAL/HIGH 消息回复 ACK
  2. LOW/NONE 消息不回复 ACK
```

### 14.6 与 KBEngine/BigWorld 的对比

| 维度 | BigWorld Mercury | KBEngine TCP | theseed |
|------|-----------------|-------------|---------|
| 内部协议 | UDP + 自建可靠性 | TCP | KCP over UDP（推荐）/ TCP（备选） |
| 可靠性分级 | 4 级（同一通道混用） | 无（全可靠） | 4 级（同一通道混用） |
| 位置更新 | RELIABLE_NO（不重传） | TCP（全重传） | NONE（不重传） |
| EntityCall | RELIABLE_CRITICAL | TCP | CRITICAL |
| 延迟 | 最低 | 较高 | 接近 Mercury |
| 实现复杂度 | 极高 | 最低 | 中等（复用 KCP） |

---

## 15. Fault Tolerance — 三级容错

> 来源：BigWorld Reviver + BackupSender + Archiver。
> 这是 KBEngine 差距最大的领域（Ch23："这是两套项目差距最大的领域"）。
> theseed 必须补上。

### 15.1 三级保障体系

```
第一级：ProcessSupervisor（进程守护）
  来源：BigWorld Reviver
  改进：theseed 用 Agent + Control Plane 替代 bwmachined

第二级：EntityBackup（跨进程热备）
  来源：BigWorld BackupSender
  KBEngine 缺失：只有 Backuper/writeBackupData，不是灾备恢复链
  theseed 改进：引入 BackupHash + 跨 BaseApp 内存热备

第三级：Archiver（周期归档）
  来源：BigWorld Archiver / KBEngine writeToDB
  改进：theseed 用 IStorageBackend 抽象 + 多后端支持
```

### 15.2 EntityBackup — 跨进程热备

```
问题：BaseApp 宕机时，上面所有实体的未写库数据全部丢失。

KBEngine 的现状：
  - Backuper 周期调用 writeBackupData
  - 但备份数据存在同一个 DBMgr，不是跨进程热备
  - 恢复时从数据库加载（慢，可能丢最近的数据）

BigWorld 的做法：
  - BackupSender 把 Base 实体数据发送到另一个 BaseApp
  - 备份在内存中（快）
  - BaseApp 宕机时从备份 BaseApp 恢复（秒级）

theseed 的设计：
```

```cpp
// runtime/EntityBackup.h

class EntityBackupManager {
public:
    // 配置备份策略
    void setBackupPolicy(const BackupPolicy& policy);

    // 注册 Base 实体的备份目标
    // BackupHash: entityId → backup BaseApp 的一致性哈希映射
    void registerBackupTarget(EntityId id, ComponentId backupBaseApp);

    // 触发备份（每个 Archiver 周期调用）
    void backupEntity(Entity* entity);

    // BaseApp 宕机时恢复
    // 从备份 BaseApp 读取实体数据，重建到新的 BaseApp
    void restoreFromBackup(ComponentId deadBaseApp,
                           ComponentId newBaseApp);

    // 查询
    bool hasBackup(EntityId id) const;
    ComponentId getBackupLocation(EntityId id) const;
};

struct BackupPolicy {
    Duration backup_interval = 5s;     // 备份间隔
    int max_backups_per_entity = 3;    // 每实体保留的备份版本数
    bool compress_backup = true;       // 是否压缩备份数据
    size_t max_backup_memory_mb = 512; // 备份 BaseApp 的最大内存使用
};
```

### 15.3 备份恢复流程

```
BaseApp_A 宕机 → 检测到死亡
  │
  ├─ 1. ControlPlane 启动新 BaseApp_A'
  │
  ├─ 2. 查询 BackupHash：BaseApp_A 上的实体 → 备份在哪些 BaseApp
  │     BackupHash: entityId → backupBaseApp
  │     例: Entity 10042 → 备份在 BaseApp_B
  │         Entity 10089 → 备份在 BaseApp_C
  │
  ├─ 3. BaseApp_A' 从各备份 BaseApp 拉取实体数据
  │     BaseApp_A' → BaseApp_B: restoreBackup(entityIds)
  │     BaseApp_A' → BaseApp_C: restoreBackup(entityIds)
  │
  ├─ 4. BaseApp_A' 反序列化实体，恢复到内存
  │     - 恢复属性
  │     - 恢复定时器
  │     - 重新建立 EntityCall 路由
  │     - 如果实体有 Cell 部分，通知 CellApp 更新 baseEntityCall
  │
  ├─ 5. 通知客户端重连到 BaseApp_A'
  │     - 客户端检测断线
  │     - LoginApp 告知新 BaseApp 地址
  │     - 客户端用 session token 恢复（不需要重新登录）
  │
  └─ 6. 更新路由表
        - BaseAppMgr 更新全局路由
        - 所有 EntityCall 持有者更新目标地址

恢复时间目标：< 5 秒（内存热备，不经过数据库）
数据丢失：最多一个 backup interval（默认 5s）
```

### 15.4 与两套引擎的对比

| 维度 | BigWorld | KBEngine | theseed |
|------|---------|---------|---------|
| 进程守护 | Reviver 自动拉起 | 无内建 | Agent + Control Plane |
| 实体热备 | BackupSender 跨进程 | 无（只有 DB 归档） | BackupHash + 跨 BaseApp |
| 恢复速度 | 秒级（内存） | 分钟级（数据库） | 秒级（内存） |
| 数据丢失 | < 1 个备份周期 | < 1 个 Archiver 周期 | < 1 个备份周期 |
| 归档 | Archiver + SecondaryDB | writeToDB + Archiver | IStorageBackend 多后端 |

---

## 16. AOI Update Schemes — 可插拔 AOI 策略

> 来源：BigWorld `aoi_update_schemes`。
> KBEngine 把 AOI 更新逻辑硬编码在 Witness/Entity 中。
> theseed 取 BigWorld 的可扩展设计。

### 16.1 为什么需要可插拔

```
不同游戏类型对 AOI 的需求不同：

MMO（千人同屏）：
  - 需要严格的 detailLevel 分级
  - 需要带宽预算管理
  - 远处实体降频同步

MOBA/射击（低延迟）：
  - 需要高频率位置同步
  - 不需要 detailLevel（视野范围固定）
  - 可靠性要求更高

房间制（小规模）：
  - 不需要 AOI（所有人互相可见）
  - 全量广播即可

如果 AOI 更新逻辑硬编码，每次换游戏类型都要改引擎。
```

### 16.2 AOI 策略接口

```cpp
// runtime/aoi/IAOIScheme.h

class IAOIScheme {
public:
    virtual ~IAOIScheme() = default;

    // 名字
    virtual const char* name() const = 0;

    // 实体进入/离开视野时的回调
    virtual void onEntityEnterView(Witness* witness,
                                    Entity* entity,
                                    float distance) = 0;
    virtual void onEntityLeaveView(Witness* witness,
                                    Entity* entity) = 0;

    // tick 末调用：决定哪些实体需要同步、同步什么
    virtual void updateView(Witness* witness,
                            Duration tickBudget,
                            Bundle& outBundle) = 0;

    // 计算实体的同步优先级
    virtual float calculatePriority(const Witness* witness,
                                     const Entity* entity) const = 0;

    // 计算 detailLevel
    virtual int calculateDetailLevel(float distance) const = 0;

    // 带宽预算
    virtual size_t estimateBundleSize(const Witness* witness) const = 0;
};
```

### 16.3 内建策略

```cpp
// runtime/aoi/schemes/MMOScheme.h
// 默认策略：来自 BigWorld + KBEngine 的融合
class MMOScheme : public IAOIScheme {
    // detailLevel 分级（来自 KBEngine 3 级 + BigWorld 4 级）
    // 带宽预算管理（来自 BigWorld EntityCache 优先级队列）
    // Hysteresis 防抖（来自 BigWorld DataLoDLevels）
    // volatile threshold（来自两边的 VolatileInfo）
};

// runtime/aoi/schemes/ActionScheme.h
// 动作游戏策略：高频同步，无 detailLevel
class ActionScheme : public IAOIScheme {
    // 固定视野范围
    // 所有可见实体全量同步
    // 高频位置更新（每 tick）
    // 无带宽预算（视野内实体数有限）
};

// runtime/aoi/schemes/RoomScheme.h
// 房间制策略：无 AOI，全量广播
class RoomScheme : public IAOIScheme {
    // 所有实体互相可见
    // 所有变更广播给所有人
    // 不需要 CoordinateSystem
};
```

### 16.4 配置

```yaml
# config/aoi.yaml

aoi:
  # 策略选择（按 Space 类型可覆盖）
  default_scheme: mmo

  # Space 级别覆盖
  space_overrides:
    - space_type: "BattleRoyale"
      scheme: action
    - space_type: "ChatRoom"
      scheme: room

  # MMO 策略配置
  mmo:
    detail_levels:
      - level: 0          # 近距离
        distance: 30       # 30 米内
        sync_props: all    # 同步所有 CLIENT_VISIBLE 属性
      - level: 1          # 中距离
        distance: 80
        sync_props: [position, rotation, name, equipment_visual]
      - level: 2          # 远距离
        distance: 200
        sync_props: [position, rotation]
    bandwidth_budget: 8192  # 每 tick 最大同步字节数
    hysteresis: 5.0         # 防抖距离
    volatile_position_threshold: 0.5  # 位置变化 > 0.5m 才同步
    volatile_rotation_threshold: 0.1  # 旋转变化 > 0.1rad 才同步

  # Action 策略配置
  action:
    view_radius: 100
    sync_frequency: every_tick
    sync_props: all
```

---

## 17. Dynamic Space Topology — 动态空间拓扑

> 来源：BigWorld BSP 树 + grow/shrink + Offload。
> KBEngine 不做动态拓扑，每个 Space 只有一个 Cell。
> theseed 初期可以简化，但接口要预留。

### 17.1 BSP 树 vs 简化 vs theseed 的选择

```
BigWorld BSP 树：
  优势：单 Space 大世界无缝地图，Cell 边界自动调整
  代价：BSP 树维护复杂，Cell 边界同步，实体迁移频繁

KBEngine 简化：
  优势：简单，一个 Space = 一个 Cell
  代价：单 CellApp 处理不了大世界，无法水平扩展

theseed 的选择（分阶段）：

Phase 1：KBEngine 模式
  - 一个 Space = 一个 Cell
  - 够用 90% 的场景（房间制/副本制/中型地图）
  - 实现简单

Phase 2：静态多 Cell
  - 手动配置 Space 的 Cell 划分
  - 每个 Cell 可以放在不同 CellApp
  - Cell 边界固定
  - 实体跨 Cell 走 Ghost + 迁移

Phase 3：动态拓扑（BigWorld 级）
  - BSP 树动态分割
  - Cell 边界自动 grow/shrink
  - 基于负载的自动均衡
  - 这需要三级 Profiler（下一节）
```

### 17.2 接口预留

```cpp
// runtime/ISpaceTopology.h

class ISpaceTopology {
public:
    virtual ~ISpaceTopology() = default;

    // 查询实体属于哪个 Cell
    virtual CellId locateCell(const Vector3& position) const = 0;

    // 查询位置的相邻 Cell（Ghost 创建需要）
    virtual std::vector<CellId> getAdjacentCells(const Vector3& pos,
                                                  float radius) const = 0;

    // 拓扑变更通知
    virtual void onTopologyChanged(std::function<void()> callback) = 0;

    // 负载上报（供动态拓扑使用）
    virtual void reportLoad(CellId cell, float load) = 0;

    // 触发重平衡（Phase 3 用）
    virtual void rebalance() = 0;
};

// Phase 1 实现
class SingleCellTopology : public ISpaceTopology {
    // 一个 Space 只有一个 Cell，所有查询都返回同一个 CellId
};

// Phase 2 实现
class StaticPartitionTopology : public ISpaceTopology {
    // 手动配置的固定分区，用 grid 或 rect 划分
};

// Phase 3 实现（远期）
class BSPTopology : public ISpaceTopology {
    // BigWorld 风格的 BSP 树，动态 grow/shrink
};
```

---

## 18. Load Balancing — 三级 Profiler 数据驱动

> 来源：BigWorld EntityTypeProfiler → EntityProfiler → CellProfiler。
> KBEngine 只有 Watcher 中的基本指标。
> theseed 融合 BigWorld 的数据驱动模型 + OTel Metrics。

### 18.1 三级负载采集

```
Level 1: EntityProfiler（每个实体）
  来源：BigWorld EntityProfiler
  采集：
    - 脚本执行时间（per method）
    - 属性同步字节数
    - EntityCall 发送频率
    - 定时器数量和执行时间
    - Ghost 数量

Level 2: EntityTypeProfiler（按实体类型聚合）
  来源：BigWorld EntityTypeProfiler
  采集：
    - 该类型实体总数
    - 该类型的平均/最大 CPU 时间
    - 该类型的总同步带宽
    - 该类型的总 EntityCall 频率

Level 3: CellProfiler（Cell 级汇总）
  来源：BigWorld CellProfiler
  采集：
    - Cell 内实体总数
    - Cell 总 CPU 时间
    - Cell 总带宽
    - Cell 健康度（tick 是否超预算）
    - Cell 内存使用
```

### 18.2 核心接口

```cpp
// runtime/LoadProfiler.h

struct EntityLoadProfile {
    EntityId entityId;
    uint16_t entityType;

    Duration scriptTime;          // 脚本执行总时间
    Duration timerTime;           // 定时器执行总时间
    size_t syncBytes;             // 同步字节数
    size_t entityCallCount;       // EntityCall 次数
    int ghostCount;               // Ghost 数量
    int timerCount;               // 定时器数量
};

struct EntityTypeLoadProfile {
    uint16_t entityType;
    std::string typeName;

    int entityCount;
    Duration avgScriptTime;
    Duration maxScriptTime;
    size_t totalSyncBytes;
    size_t totalEntityCallCount;
};

struct CellLoadProfile {
    CellId cellId;
    SpaceId spaceId;

    int entityCount;
    int playerCount;              // 有客户端的实体数
    Duration tickTime;            // 总 tick 时间
    Duration scriptTime;          // 总脚本时间
    Duration aoiTime;             // AOI 更新时间
    size_t totalSyncBytes;
    size_t totalMemoryBytes;
    float healthScore;            // 0-100，综合健康度
};

class LoadProfiler {
public:
    // 采集（每个 tick 末调用）
    void collectEntity(Entity* entity, const TickProfile& tick);
    void aggregateByType();
    void aggregateByCell();

    // 查询
    const EntityLoadProfile& getEntityProfile(EntityId id) const;
    const EntityTypeLoadProfile& getTypeProfile(uint16_t type) const;
    const CellLoadProfile& getCellProfile(CellId id) const;

    // 导出（自动对接 OTel Metrics）
    void exportMetrics();
};
```

### 18.3 数据驱动的自动均衡（Phase 3）

```
每个 tick 末：

1. 采集三级 Profiler 数据
   → EntityLoadProfile → EntityTypeLoadProfile → CellLoadProfile

2. 汇报到 CellAppMgr
   → CellAppMgr 拥有全局视图

3. 判定是否需要均衡
   → Cell A 负载 > 阈值？
   → Cell B 负载 < 阈值？
   → 均衡收益 > 迁移成本？

4. 决定迁移方案（如果需要）
   → 选哪些实体迁移（EntityTypeProfiler 告诉你哪种类型最重）
   → 迁移到哪里（CellProfiler 告诉你哪个 Cell 最轻）
   → 移多少（负载差 vs 迁移成本）

5. 执行迁移
   → 走 Section 8 的迁移流程

全程数据驱动，不靠人工判断。
```

---

## 19. Login Security — 登录安全

> 来源：BigWorld Login Challenge (Cuckoo Cycle PoW)。
> KBEngine 只有 Blowfish + rndUUID。
> theseed 借鉴 BigWorld 的 PoW 反 DDoS 思路。

### 19.1 登录流程安全

```
theseed 登录安全多层防护：

Layer 1: 网络层
  - Gateway TLS 终结
  - IP 限流（令牌桶）
  - IP 黑白名单

Layer 2: Challenge-Response（来自 BigWorld）
  - 客户端请求登录 → 服务器返回 Challenge
  - 客户端必须解 Challenge（消耗计算资源）
  - Challenge 难度可调（攻击时提高难度）
  - 有效防止暴力登录和 DDoS

Layer 3: Token 认证
  - Challenge 通过后，服务器签发 JWT token
  - 后续请求携带 token
  - token 有过期时间，可撤销

Layer 4: 会话绑定
  - token 绑定 IP（可选）
  - token 绑定设备指纹（可选）
  - 防止 token 被盗用
```

### 19.2 Challenge 机制

```cpp
// gateway/LoginChallenge.h

class ILoginChallenge {
public:
    virtual ~ILoginChallenge() = default;

    // 生成 Challenge
    virtual Challenge generate(const std::string& clientIp) = 0;

    // 验证 Challenge 应答
    virtual bool verify(const Challenge& challenge,
                        const ChallengeResponse& response) = 0;

    // 调整难度（根据服务器负载）
    virtual void adjustDifficulty(float serverLoad) = 0;
};

// 简单 PoW 实现（非 Cuckoo Cycle，够用）
class HashPoW : public ILoginChallenge {
    // Challenge: 随机 nonce + difficulty (前导零位数)
    // Response: 找到一个值使得 SHA256(challenge + response) 满足 difficulty
    // 正常客户端 < 100ms，攻击者需要大量计算
};
```

### 19.3 与两套引擎的对比

| 维度 | BigWorld | KBEngine | theseed |
|------|---------|---------|---------|
| 登录 Challenge | Cuckoo Cycle PoW | 无 | HashPoW（可插拔） |
| 加密 | 可插拔加密层 | Blowfish | TLS（Gateway 层） |
| 认证 | 脚本层实现 | rndUUID + account | JWT + Challenge |
| 反 DDoS | Challenge 提高难度 | 无 | Challenge + IP 限流 |

---

## 20. BigWorld → theseed 强项融合总表

```
来自 BigWorld 的强项（theseed 必须继承）：

  1. ✅ Base/Cell 双身体模型         → Section 3
  2. ✅ 十字链表 AOI                 → Section 4
  3. ✅ Ghost real/ghost 权威模型    → Section 5
  4. ✅ Witness + detailLevel + Hysteresis → Section 4.4
  5. ✅ 四级消息可靠性（Mercury）      → Section 14（用 KCP 替代自建）
  6. ✅ 三级容错（Reviver/BackupSender/Archiver） → Section 15
  7. ✅ aoi_update_schemes 可插拔策略  → Section 16
  8. ✅ BSP 动态拓扑                  → Section 17（Phase 3 预留）
  9. ✅ 三级 Profiler 负载均衡         → Section 18
  10. ✅ Login Challenge 反 DDoS      → Section 19
  11. ✅ 优先级队列 + 带宽预算（Witness） → Section 16 MMOScheme

来自 KBEngine 的强项（theseed 必须继承）：

  1. ✅ tick 5 阶段分段               → Section 2
  2. ✅ EntityCall 单向简化            → Section 7
  3. ✅ GhostManager 路由窗口          → Section 5.4 + 8.3
  4. ✅ 对象池模式                     → Section 10
  5. ✅ exposed 方法信任边界           → Section 7 + 04-client-sdk.md
  6. ✅ aliasID 带宽优化               → Section 6.3
  7. ✅ Volatile 属性阈值同步          → Section 4 + 6

theseed 自己的改进（两者都没有）：

  1. ✅ PropertyBlock 连续内存         → Section 3.3
  2. ✅ DirtyMask 位图                → Section 6.2
  3. ✅ OTel 可观测性                  → 02-observability.md
  4. ✅ 客户端代码生成器               → 04-client-sdk.md
  5. ✅ 脚本安全四层模型               → 05-script-security.md
  6. ✅ 多存储后端抽象                 → 01-developer-experience.md
  7. ✅ EntityBackup 跨进程热备        → Section 15（融合 BigWorld 思想）
  8. ✅ ISpaceTopology 分阶段演进      → Section 17
```

---

## 21. 更新后的实现优先级

```
P0 - 地基（没有这些引擎跑不起来）
  1. TickScheduler（主循环）
  2. Entity + PropertyBlock（实体核心）
  3. EntityCall + ReliableTransport（进程间通信 + 可靠性分级）
  4. CoordinateSystem + CoordinateNode（AOI 索引）
  5. RangeTrigger（AOI 触发器）
  6. Witness + IAOIScheme（视野系统 + 可插拔策略）
  7. PropertySync + DirtyMask（属性同步）
  8. TimerWheel（定时器）

P1 - 核心（引擎可用）
  9. GhostManager（Ghost 系统）
  10. MigrationManager（实体迁移）
  11. VolatileInfo（位置/朝向同步优化）
  12. ObjectPool + BundlePool（内存管理）
  13. LoadProfiler 三级采集（负载监控）

P2 - 生产（引擎可上线）
  14. EntityBackup 跨进程热备（容错）
  15. ProcessSupervisor（进程守护）
  16. detailLevel + aliasID 优化（带宽节省）
  17. LoginChallenge（安全）
  18. ArenaAllocator（内存连续性）

P3 - 高级（大规模场景）
  19. StaticPartitionTopology（静态多 Cell）
  20. 数据驱动负载均衡
  21. ActionScheme / RoomScheme（多游戏类型支持）

P4 - 远期（BigWorld 级）
  22. BSPTopology（动态拓扑）
  23. Cell 边界自动 grow/shrink
```
