# Controllers — Entity Behavior Controllers

> **English** | [中文](03-controllers.zh.md)

> Controller is the core abstraction for the server to drive entity behavior.
>
> Source of truth: the behavior abstraction of BigWorld's 30+ Controller types.
> Reference implementation: the concise interfaces of KBEngine's 4 Controller types.
> theseed takes BigWorld's capability boundary and KBEngine's lightweight implementation approach.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
BigWorld's Controller system is very complete:
  - Movement
  - Facing
  - Patrol
  - Chase
  - Vision / proximity style behavior control
```

### How KBEngine Implements It

```
KBEngine only retains a small number of high-value Controllers:
  - Lighter interfaces
  - Emphasizes practicality over type completeness
```

### Pros and Cons

```
BigWorld's advantages:
  - Broad capability coverage
  - Complete behavior abstraction

KBEngine's advantages:
  - Simple
  - Easy to implement first

Common disadvantages:
  - Once there are too many Controllers, migration and serialization costs rise
```

### theseed's Trade-offs

```
theseed does not pursue porting BigWorld's full Controller set from the start,
but first defines a unified interface and a migratable boundary,
then adds types gradually by business value.
```

---

## 1. IController Base Class

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

## 2. Built-in Controllers

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

`pause/resume` only indicates the controller's own running state,
and does not mean pausing/resuming a script debugging session.

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

## 4. Script Layer API

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

## 5. Comparison

| Capability | BigWorld | KBEngine | theseed |
|------|----------|----------|---------|
| Move to point | MoveToPointController | None | MoveController::moveToPoint |
| Move to entity | MoveToEntityController | None | MoveController::moveToEntity |
| Navigation movement | NavigationController | Entity.navigate() | NavigateController |
| Proximity trigger | ProximityController | ProximityController | ProximityController |
| Face entity | FaceEntityController | None | FaceController |
| Serialization/migration | writeRealToStream | None | serialize/deserialize |
