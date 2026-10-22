# Built-in Entities — Built-in Game Objects

> English | **中文**

> BigWorld/KBEngine only provide the three base classes Entity/Proxy/Space; all game objects are implemented from scratch.
> theseed builds in common game objects: DroppedItem, Projectile, Monster, NPC, Trigger, Vehicle, Pet, SpawnPoint.
> C++ core logic + script-overridable hooks, usable with zero configuration.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
BigWorld leans more toward providing low-level abstractions:
  - Entity
  - Proxy
  - Space
  - Mechanisms such as Controller / Trigger

Specific game objects are usually implemented by the project itself.
```

### How KBEngine Implements It

```
KBEngine is also closer to a basic framework
and does not build in too many specific game object types.
```

### Pros and Cons

```
BigWorld / KBEngine's advantages:
  - The engine core stays generic
  - Does not force-bind specific gameplay types

Common disadvantages:
  - High project startup cost
  - A large number of common objects get reinvented repeatedly
```

### theseed's Trade-offs

```
theseed chooses to build in a small number of high-frequency objects,
but keeps the count restrained,
to avoid turning the engine into a project-specific gameplay library.
```

---

## 1. Design Principles

```
1. C++ core logic + script-overridable hooks
2. Usable with zero configuration (effective on creation, with built-in lifecycle management)
3. Inheritable and overridable
4. Restrained count (only builds in common types that cover 90% of game scenarios)
```

---

## 2. Type Hierarchy

```
Entity (C++ core)
├── BaseEntity
│   ├── Account                    # Account entity
│   └── GlobalEntity               # Global singleton
├── CellEntity
│   ├── Player                     # Player character
│   ├── Creature                   # Creature base class
│   │   ├── NPC                    # NPC (interaction, shop, quests)
│   │   └── Monster                # Monster (AI state machine, drops)
│   ├── DroppedItem                # Dropped item
│   ├── Projectile                 # Projectile entity
│   ├── Trigger                    # Trigger zone
│   ├── Vehicle                    # Vehicle / mount
│   ├── Pet                        # Pet / summon
│   └── SpawnPoint                 # Spawn point
└── Space
```

---

## 3. DroppedItem (Dropped Item)

```python
class DroppedItem(CellEntity):
    def onAppear(self): ...                    # 物品落地完成
    def onPickup(self, playerId) -> bool: ...  # 拾取（返回 True 允许）
    def onExpire(self): ...                    # 超时消失
    def onDestroy(self): ...                   # 销毁

# 一行创建
theseed.createEntity("DroppedItem", {
    "itemId": 10042, "count": 1, "position": dropPos,
    "lifetime": 300, "ownerId": killerId, "ownerProtectTime": 60,
})
```

---

## 4. Projectile

```python
class Projectile(CellEntity):
    def onLaunch(self): ...                             # 发射瞬间
    def onHitEntity(self, targetId) -> bool: ...       # 命中实体（True=穿透）
    def onHitObstacle(self, hitPoint, hitNormal): ...  # 命中障碍物
    def onReachMaxRange(self): ...                      # 最大射程
    def onTimeout(self): ...                            # 超时

# 追踪型
theseed.createEntity("Projectile", {
    "sourceId": casterId, "targetId": targetId,
    "speed": 30.0, "damage": 100, "aoeRadius": 3.0,
})

# 定点型
theseed.createEntity("Projectile", {
    "sourceId": casterId, "targetPos": targetPos,
    "speed": 20.0, "penetration": 2,
})
```

---

## 5. Creature / Monster / NPC

```python
class Creature(CellEntity):
    """生物基类"""
    def onSpawn(self): ...
    def onDeath(self, killerId): ...
    def onRespawn(self): ...
    def onHpChanged(self, oldHp, newHp): ...
    def onBuffApplied(self, buffId, stacks): ...
    def onDamageDealt(self, targetId, damage): ...
    def onDamageTaken(self, sourceId, damage): ...

class Monster(Creature):
    """怪物 — 自带 AI 状态机"""
    def onIdle(self): ...
    def onPatrol(self): ...
    def onAggro(self, targetId): ...
    def onLeash(self): ...           # 脱战返回
    def onFlee(self): ...
    def onKillPlayer(self, playerId): ...

class NPC(Creature):
    """NPC — 自带交互、对话、商店"""
    def onInteract(self, playerId): ...
    def onDialogStart(self, playerId, dialogId): ...
    def onShopOpen(self, playerId): ...
    def onQuestAccept(self, playerId, questId): ...
```

---

## 6. Trigger / Vehicle / Pet / SpawnPoint

```python
class Trigger(CellEntity):
    """场景触发区域"""
    def onEntityEnter(self, entityId): ...
    def onEntityLeave(self, entityId): ...
    def onEntityStay(self, entityId, deltaTime): ...

class Vehicle(CellEntity):
    """载具/坐骑"""
    def onMount(self, playerId) -> bool: ...
    def onDismount(self, playerId): ...

class Pet(CellEntity):
    """宠物/召唤物"""
    def onSummon(self, ownerId): ...
    def onRecall(self): ...
    def onCommand(self, command, targetId): ...

class SpawnPoint(CellEntity):
    """刷怪点"""
    def onSpawn(self, entityList): ...
    def onAllKilled(self): ...
    def onRespawnTimer(self): ...
```

---

## 7. Account

```python
class Account(BaseEntity):
    """Base 侧账号实体"""
    def onClientConnected(self): ...
    def onClientDisconnected(self): ...
    def onClientReconnected(self): ...
    def onCharacterSelected(self, characterId): ...
    def onCharacterCreated(self, characterData): ...
    def onCharacterDeleted(self, characterId): ...
```

---

## 8. Comparison with BigWorld/KBEngine

```
Game Object Type          BigWorld                  KBEngine                theseed
───────────────────────────────────────────────────────────────────────────────────
Dropped Item              ❌                        ❌                      ✅ DroppedItem
Projectile                ❌                        ❌                      ✅ Projectile
NPC                       ❌                        ❌                      ✅ NPC
Monster (with AI)         ❌(script template)       ❌(script template)     ✅ Monster
Trigger Zone              ❌(ProxCtrl underlying)   ❌(addProx underlying)  ✅ Trigger
Vehicle/Mount             ❌(PassengerCtrl)         ❌                      ✅ Vehicle
Pet/Summon                ❌                        ❌                      ✅ Pet
Spawn Point               ❌                        ❌                      ✅ SpawnPoint
Account                   ❌(script impl)           ✅ Account              ✅ Account
Creature Base             ❌                        ❌                      ✅ Creature
```
