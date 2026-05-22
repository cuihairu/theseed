# Controllers — 实体行为控制器

> Controller 是服务端驱动实体行为的核心抽象。
>
> 来源：BigWorld 30+ 种 Controller，KBEngine 4 种。
> theseed 取 BigWorld 的丰富性 + KBEngine 的简洁接口。

---

## 0.5 引擎实现对照与取舍

### BigWorld 是怎么实现的

```
BigWorld 的 Controller 体系很完整：
  - 移动
  - 朝向
  - 巡逻
  - 追踪
  - 视野 / proximity 一类行为控制
```

### KBEngine 是怎么实现的

```
KBEngine 只保留了少量高价值 Controller：
  - 接口更轻
  - 更强调实用而不是类型完备
```

### 优缺点

```
BigWorld 的优点：
  - 能力覆盖面广
  - 行为抽象完整

KBEngine 的优点：
  - 简单
  - 容易先实现

共同缺点：
  - Controller 一旦过多，迁移和序列化成本会上升
```

### theseed 的取舍

```
theseed 不追求一开始复制 BigWorld 全量 Controller，
而是先定义统一接口和可迁移边界，
再按业务价值逐步补类型。
```

---

## 1. IController 基类

```cpp
class IController {
public:
    virtual void onStart() = 0;
    virtual void onStop() = 0;
    virtual void onCancel() = 0;
    virtual void onUpdate(float deltaTime) = 0;

    virtual void serialize(MemoryStream& out) const = 0;
    virtual void deserialize(MemoryStream& in) = 0;

    virtual ControllerId id() const = 0;
    virtual std::string typeName() const = 0;
    virtual bool isRunning() const = 0;
};
```

---

## 2. 内建 Controller

### MoveController

```cpp
class MoveController : public IController {
public:
    static std::shared_ptr<MoveController> moveToPoint(
        Entity& entity, const Vector3& target, float speed,
        MovementCallback onComplete = nullptr);

    static std::shared_ptr<MoveController> moveToEntity(
        Entity& entity, EntityId targetId, float speed,
        float stopDistance = 1.0f,
        MovementCallback onComplete = nullptr);

    void setSpeed(float speed);
    void pause();
    void resume();
};
```

`pause/resume` 只表示控制器自身运行状态，
不等于脚本调试会话的暂停/继续。

### NavigateController

```cpp
class NavigateController : public IController {
public:
    static std::shared_ptr<NavigateController> navigateToPoint(
        Entity& entity, const Vector3& target, float speed,
        int layer = 0,
        MovementCallback onComplete = nullptr);
};
```

### ProximityController

```cpp
class ProximityController : public IController {
public:
    using ProximityCallback = std::function<void(EntityId, bool /* entered */)>;
    static std::shared_ptr<ProximityController> create(
        Entity& entity, float innerRadius, float outerRadius,
        ProximityCallback onProximity);
};
```

### FaceController

```cpp
class FaceController : public IController {
public:
    static std::shared_ptr<FaceController> faceEntity(
        Entity& entity, EntityId targetId, float rotateSpeed = 10.0f);
};
```

---

## 3. ControllerManager

```cpp
class ControllerManager {
public:
    ControllerId add(std::shared_ptr<IController> controller);
    void cancel(ControllerId id);
    void cancelAll();
    void cancelByType(const std::string& typeName);

    IController* get(ControllerId id) const;
    std::vector<IController*> getByType(const std::string& typeName) const;

    void tick(float deltaTime);
    void serialize(MemoryStream& out) const;
    void deserialize(MemoryStream& in);
};
```

---

## 4. 脚本层 API

```python
class Monster(BaseEntity):
    def onProximityEnter(self, otherId, entered):
        if entered:
            self.controllers.cancelAll()
            self.controllers.moveToEntity(
                otherId, speed=6.0, stopDistance=2.0,
                onComplete=self.onReachTarget
            )
            self.controllers.faceEntity(otherId)

    def onReachTarget(self, entityId, reached):
        if reached:
            self.attack(entityId, SKILL_MELEE)
```

---

## 5. 对比

| 能力 | BigWorld | KBEngine | theseed |
|------|----------|----------|---------|
| 移动到点 | MoveToPointController | 无 | MoveController::moveToPoint |
| 移动到实体 | MoveToEntityController | 无 | MoveController::moveToEntity |
| 导航移动 | NavigationController | Entity.navigate() | NavigateController |
| 范围触发 | ProximityController | ProximityController | ProximityController |
| 面向实体 | FaceEntityController | 无 | FaceController |
| 序列化/迁移 | writeRealToStream | 无 | serialize/deserialize |
