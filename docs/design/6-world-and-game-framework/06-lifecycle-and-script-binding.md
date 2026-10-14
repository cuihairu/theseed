# Lifecycle & Script Binding — Lifecycle Hooks and Script Binding

> **English** | [中文](06-lifecycle-and-script-binding.zh.md)

> theseed's unified entity lifecycle hook design and the C++ ↔ script binding architecture.
>
> Source of truth: BigWorld's 30+ lifecycle hooks and hand-written PyScript bindings.
> Reference implementation: KBEngine's 20+ hooks and hand-written Python C API bindings.
> The current implementation baseline follows [../0-foundation/01-mvp-architecture-baseline](../0-foundation/01-mvp-architecture-baseline.md).

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
BigWorld has a large number of lifecycle hooks and hand-written script bindings:
  - Rich hooks
  - High binding freedom
  - But the boundaries and timing need to be mastered by the developer
```

### How KBEngine Implements It

```
KBEngine also has relatively complete lifecycle hooks,
and likewise uses the hand-written binding route,
but overall it is somewhat more restrained than BigWorld.
```

### Pros and Cons

```
BigWorld's advantages:
  - Strong expressiveness
  - Complete hook coverage

KBEngine's advantages:
  - More restrained
  - Lower implementation complexity

Common disadvantages:
  - The more hooks there are, the easier it is to misunderstand the timing
  - Once bindings have no boundary, scripts can easily bypass runtime rules
```

### theseed's Trade-offs

```
theseed continues to keep the lifecycle hook main line,
but explicitly writes out the tick thread, property synchronization, hot update, and migration boundaries,
and does not allow "rich hooks" to evolve into "blurred boundaries".
```

---

## 1. Design Principles

```
1. Hooks only express "when something happened"; they do not hide thread and timing boundaries
2. Hook callbacks are always executed on the owning tick thread
3. Hooks cannot bypass the tick synchronization rules and send property packages directly
4. Hot update only guarantees new entry logic; it cannot assume the old call stack is automatically compatible
5. Migration does not promise script stack recovery; it only recovers serializable runtime state
```

---

## 2. Entity Lifecycle Hooks (Implementation Baseline)

```
                          BigWorld             KBEngine                theseed
                          ─────────            ──────────              ─────────
Creation
Base creation             __init__()           __init__()              onCreate()
Cell creation             onCellCreated()      onEnterCell()           onCellCreated()
Restore (archive/backup)  onRestore()          onRestore()             onRestore()

Entering the world
Base enters world         onEnterWorld()       (always in world)       onEnterWorld()
Cell enters world         onEnterWorld()       onEnterWorld()          onEnterWorld()
Enters Space              onEnterSpace()       onEnterSpace()          onEnterSpace()

Client connection
Gains player control      onBecomePlayer()     onGetCell()             onBecomePlayer()
Loses player control      onLosePlayer()       onLoseCell()            onLosePlayer()
Client comes online       onLogOn()            onClientEnabled()       onClientConnected()
Client disconnects        onClientDeath()      onClientDeath()         onClientDisconnected()
Client reconnects         (no dedicated)       onClientEnabled()       onClientReconnected()

Ghost related
Becomes Real              (always known)       onBecomeReal()          onBecomeReal()
Becomes Ghost             (always known)       onBecomeGhost()         onBecomeGhost()
Seen by Witness           onWitnessed()        (none)                  onWitnessed()
No longer seen            onUnwitnessed()      (none)                  onUnwitnessed()

AOI related
Entity enters view        onEnterAOI()         onEnterWorld()          onEntityEnterAOI()
Entity leaves view        onLeaveAOI()         onLeaveWorld()          onEntityLeaveAOI()

Property change
Property modified         onPropertyChange()   onChanged()             onPropertyChanged()

Migration
Before migration          onBeforeMigrate()    (none)                  onBeforeMigrate()
After migration           onAfterMigrate()     (none)                  onAfterMigrate()
Teleport                  onTeleport()         onTeleportSuccess()     onTeleport()

Destruction
Leaves Space              onLeaveSpace()       onLeaveSpace()          onLeaveSpace()
Leaves world              onLeaveWorld()       onLeaveWorld()          onLeaveWorld()
Cell destroyed            onCellDestroyed()    onLeaveCell()           onCellDestroyed()
Base destroyed            onDestroy()          onDestroy()             onDestroy()

Timer
Timer callback            onTimer(id,userArg)  onTimer(id,userArg)     onTimer(id,userArg)

Database
Write to database         onWriteToDB()        onWriteToDB()           onWriteToDB()
Load from database        onReadFromDB()       (implicit in __init__)  onLoadFromDB()
```

---

## 3. Hook Boundary Explanation

### 3.1 onPropertyChanged

```
onPropertyChanged means "the property was modified in the current tick".

It does not mean:
  - The property has been synchronized to the client
  - The property has been synchronized to the ghost
  - The property has been persisted to the database

Synchronization is still executed in a unified flush at the end of the tick.
```

### 3.2 onBeforeMigrate / onAfterMigrate

```
These two hooks can only rely on serializable runtime state.

Cannot assume:
  - The current script call stack will be carried over
  - A continuation suspended by yield will automatically resume
  - An old-version script frame will be fully compatible with the code at the new location
```

### 3.3 Hook Semantics Under Hot Update

```
Restricted L2 hot update only guarantees that "newly entered call paths" use the new implementation.

Does not guarantee:
  - An old script frame currently executing switches to the new logic immediately
  - Already-registered callbacks automatically acquire the new closure semantics

Therefore:
  Hot update is suitable for replacing entry logic, not for modifying long-lifecycle script state machine protocols.
```

For the debugging interaction boundary, see `../7-scripting-and-client/03-script-debug`:
hooks can be paused and single-stepped, but pause points may only land on the callback entry and must not be inserted into the C++ core synchronization path.

---

## 4. Script Binding Layer Design

### 4.1 Design Principles

```
1. Zero-cost abstraction: the C++ core path does not go through scripts (Tick, AOI, Ghost synchronization → pure C++)
2. Minimal exposure surface: only expose necessary interfaces (Entity properties, EntityCall, Controller, navigation/physics, DB, timers)
3. Type safety: types defined in def automatically generate strongly typed bindings
4. Does not pursue automatic binding of the full API: hand-written high-quality core bindings
```

### 4.2 Binding Architecture

```
┌──────────────────────────────────────────────────────────────────────────────┐
│  Script Layer (Python / Lua)                                                 │
│  BaseEntity / CellEntity / Space                                             │
│  theseed.navigation / theseed.physics / theseed.db                           │
├──────────────────────────────────────────────────────────────────────────────┤
│  Binding Layer                                                               │
│  EntityBinding     → Property/method binding (hand-written/generated hybrid) │
│  ControllerBinding → Controller factory binding                              │
│  NavigationBinding → INavigationSystem binding                               │
│  PhysicsBinding    → IPhysicsQuery binding                                   │
│  StorageBinding    → IEntityStore / Query binding                            │
├──────────────────────────────────────────────────────────────────────────────┤
│  C++ Core                                                                    │
│  Entity / Controller / Navigation / Physics / Space                          │
└──────────────────────────────────────────────────────────────────────────────┘
```

### 4.3 def-Driven Automatic Binding

```cpp
// Avatar.def 中声明了 <property name="hp" type="FLOAT32" flags="OWN_CLIENT"/>
// 自动生成的绑定：
class AvatarBinding : public CellEntityBinding {
public:
    float get_hp() const {
        return entity_.propertyBlock().getFloat(PropertyIndex::hp);
    }

    void set_hp(float value) {
        entity_.propertyBlock().setFloat(PropertyIndex::hp, value);
        entity_.markDirty(PropertyIndex::hp);
    }
};
```

---

## 5. Controller Lifecycle

```
                         BigWorld             KBEngine       theseed
                         ─────────            ──────────     ─────────
Creation                 create()             (no explicit)  onStart()
Per-frame update         updatable tick       tick()         onUpdate(dt)
Complete/arrival         standardCallback()   (none)         onComplete()
Cancel                   onCancel()           cancel()       onCancel()
Entity becomes Ghost     stopReal()           (none)         onPause()
Entity becomes Real      startReal()          (none)         onResume()
Migration serialization  writeRealToStream()  (none)         serialize()
```

```
MVP constraints:
  - Controller must only rely on serializable state
  - Irrecoverable script closures are not allowed to be treated as necessary migration state
```
