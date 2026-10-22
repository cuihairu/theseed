# Ghost & Witness — Cross-Boundary Perception and the View System

> English | **中文**

> Ghost solves the visibility problem of entities crossing Cell boundaries; Witness solves the view synchronization from entities to clients.
> The two are the most critical runtime mechanisms on top of the AOI system.
>
> Origin: BigWorld `Witness / RealEntity / EntityCache / Ghost / Haunt`.
> KBEngine is a lightweight reference implementation that inherits the real/ghost and Witness backbone, but with a thinner system periphery.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
BigWorld makes Witness and Ghost core runtime mechanisms:
  - Witness is responsible for the client view and data packing
  - Ghost is responsible for neighbor visibility across Cells
  - EntityCache is responsible for the detail level, priority, and client state of each observation relationship
  - AOI, detail level, priority update, and bandwidth budget form one chain
```

### How KBEngine Implements It

```
KBEngine retains the same semantic backbone:
  - Witness is responsible for view synchronization
  - Ghost is responsible for cross-boundary perception
  - But the overall system periphery is lighter than BigWorld's
```

### Pros and Cons

```
Common advantages:
  - Semantics directly fit MMO runtime
  - Cross-boundary and client synchronization are modeled uniformly

Common disadvantages:
  - More complex than a pure event broadcast model
  - Requires strong coupling with AOI / replication protocol
```

### theseed's Trade-offs

```
theseed inherits this model,
because it is more suitable for entity-based game servers than "pure replication / pure subscription".
The cost is that AOI, property synchronization, migration windows, and per-witness synchronization state must be designed together.
Among them, BigWorld's `EntityCache` semantics cannot be lost; it is only that the MVP is not required to implement them fully right away.
```

---

## 1. Witness: The View System

### 1.1 Core Interface

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

### 1.2 Core Logic of Witness::tick()

```
Witness::tick() — executed at the end of each tick:

1. Handle view enter/leave events
   onEnterView → send createEntity message to the client
                 → record into viewEntities_
   onLeaveView → send leaveEntity message to the client
                 → remove from viewEntities_

2. Update detailLevel
   For each viewEntity:
     compute distance
     distance < near_threshold → detailLevel = 0
     distance < far_threshold  → detailLevel = 1
     else                      → detailLevel = 2
   If detailLevel changed:
     From a lower level up to a higher level → send the properties of the new level
     From a higher level down to a lower level → no need to send (the client cleans up on its own)

3. Broadcast dirty properties
   For each viewEntity:
     check changedProps[detailLevel]
     Has dirty properties → serialize into Bundle → send to the client

4. Handle position/orientation updates (Volatile)
   For each viewEntity:
     Position change > threshold → send position update
     Orientation change > threshold → send orientation update

5. flush Bundle
   Send all accumulated messages to the client through the Channel
```

---

## 2. The Ghost System

### 2.1 Why Ghost Is Needed

```
Problem:
  Space A is on CellApp 1, Space B is on CellApp 2
  Entity X is in Space A, but X's AOI extends into Space B
  A player on Space B should see X appear in their view

  Without Ghost:
    Space B does not know X exists → AOI events cannot trigger → the player cannot see X

  With Ghost:
    Create a read-only replica of X on Space B (Ghost X)
    Ghost X participates in Space B's AOI → triggers work normally
    The player sees X's state through Ghost X
```

### 2.2 The Real / Ghost Authority Model

```
Core principle: single write authority

  Real Entity:
    - Has full properties
    - Has write permission
    - Handles all logic
    - Location: the CellApp where the real resides

  Ghost Entity:
    - Has partial properties (only those synchronized over)
    - Read-only
    - Does not handle logic
    - Method calls are forwarded to the Real
    - Location: other CellApps covered by AOI

  Real → Ghost sync: when properties change, the real actively pushes to the ghost
  Ghost → Real: method calls are forwarded through RealEntityMethod
```

### 2.3 Core Interface

```cpp
// runtime/GhostManager.h

class GhostManager {
public:
    // Real 侧操作
    void createGhost(ComponentId targetCellApp);
    void destroyGhost(ComponentId targetCellApp);
    void syncToGhost(Bundle& bundle);

    // Ghost 侧操作
    void onGhostSync(Bundle& bundle);
    void forwardToReal(const std::string& method,
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

### 2.4 Ghost Message Forwarding

```
Method call flow on a Ghost (from KBEngine RealEntityMethod):

1. The script calls methodA(args) on the Ghost
2. GhostManager finds that it is a ghost (isReal() == false)
3. Constructs a forwarding Bundle:
   - Target: realCell_ (the CellApp where the real resides)
   - Message: onGhostMessage
   - Content: entityID + methodName + args
4. Sends to the CellApp where the real resides
5. After the Real receives it, it executes the actual methodA(args)
6. If the method produces property changes → goes through onDefDataChanged normally
   → triggers real → ghost synchronization
```
