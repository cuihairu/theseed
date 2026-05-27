# Ghost & Witness — 跨边界感知与视野系统

> Ghost 解决实体跨 Cell 边界的可见性问题；Witness 解决实体到客户端的视野同步。
> 两者是 AOI 系统之上最关键的运行时机制。
>
> 来源源头：BigWorld `Witness / RealEntity / EntityCache / Ghost / Haunt`。
> KBEngine 是轻量参考实现，继承 real/ghost 与 Witness 主干，但系统配套更薄。

---

## 0.5 引擎实现对照与取舍

### BigWorld 是怎么实现的

```
BigWorld 把 Witness 和 Ghost 做成核心运行时机制：
  - Witness 负责客户端视野与数据打包
  - Ghost 负责跨 Cell 的邻居可见性
  - EntityCache 负责每个观察关系的 detail level、priority、client state
  - AOI、detail level、priority update、bandwidth budget 串成一条链
```

### KBEngine 是怎么实现的

```
KBEngine 保留了同样的语义主干：
  - Witness 负责视野同步
  - Ghost 负责跨边界感知
  - 但整体系统配套比 BigWorld 更轻
```

### 优缺点

```
共同优点：
  - 语义直接贴合 MMO 运行时
  - 跨边界和客户端同步统一建模

共同缺点：
  - 比纯事件广播模型复杂
  - 需要和 AOI / 复制协议强耦合
```

### theseed 的取舍

```
theseed 继承这个模型，
因为它比“纯复制/纯订阅”更适合实体型游戏服务器。
代价是必须把 AOI、属性同步、迁移窗口和 per-witness 同步状态一起设计。
其中 BigWorld 的 `EntityCache` 语义不能丢，只是不要求 MVP 立即完整实现。
```

---

## 1. Witness：视野系统

### 1.1 核心接口

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

### 1.2 Witness::tick() 核心逻辑

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

## 2. Ghost 系统

### 2.1 为什么需要 Ghost

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

### 2.2 Real / Ghost 权威模型

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

### 2.3 核心接口

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

### 2.4 Ghost 消息转发

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
