# Controllers — 实体行为控制器

> Controller 是服务端驱动实体行为的核心抽象。
>
> 来源：BigWorld 30+ 种 Controller，KBEngine 4 种。
> theseed 取 BigWorld 的丰富性 + KBEngine 的简洁接口。

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
