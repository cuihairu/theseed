# Built-in Entities — 内建游戏对象

> BigWorld/KBEngine 只提供 Entity/Proxy/Space 三个基础类，所有游戏对象从零实现。
> theseed 内建常见游戏对象：DroppedItem、Projectile、Monster、NPC、Trigger、Vehicle、Pet、SpawnPoint。
> C++ 核心逻辑 + 脚本可覆盖钩子，零配置可用。

---

## 1. 设计原则

```
1. C++ 核心逻辑 + 脚本可覆盖钩子
2. 零配置可用（创建即生效，自带生命周期管理）
3. 可继承可覆盖
4. 数量克制（只内建覆盖 90% 游戏场景的常见类型）
```

---

## 2. 类型层级

```
Entity (C++ 核心)
├── BaseEntity
│   ├── Account                    # 账号
│   └── GlobalEntity               # 全局单例
├── CellEntity
│   ├── Player                     # 玩家角色
│   ├── Creature                   # 生物基类
│   │   ├── NPC                    # NPC（交互、商店、任务）
│   │   └── Monster                # 怪物（AI 状态机、掉落）
│   ├── DroppedItem                # 掉落物品
│   ├── Projectile                 # 投射物
│   ├── Trigger                    # 触发区域
│   ├── Vehicle                    # 载具/坐骑
│   ├── Pet                        # 宠物/召唤物
│   └── SpawnPoint                 # 刷怪点
└── Space
```

---

## 3. DroppedItem（掉落物品）

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

## 4. Projectile（投射物）

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

## 7. Account（账号）

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

## 8. 与 BigWorld/KBEngine 对比

```
游戏对象类型         BigWorld        KBEngine        theseed
───────────────────────────────────────────────────────────────
掉落物品             ❌              ❌              ✅ DroppedItem
投射物               ❌              ❌              ✅ Projectile
NPC                  ❌              ❌              ✅ NPC
怪物(带AI)           ❌(脚本模板)    ❌(脚本模板)     ✅ Monster
触发区域             ❌(ProxCtrl底层) ❌(addProx底层) ✅ Trigger
载具/坐骑            ❌(PassengerCtrl) ❌            ✅ Vehicle
宠物/召唤            ❌              ❌              ✅ Pet
刷怪点               ❌              ❌              ✅ SpawnPoint
账号                 ❌(脚本实现)    ✅ Account       ✅ Account
生物基类             ❌              ❌              ✅ Creature
```
