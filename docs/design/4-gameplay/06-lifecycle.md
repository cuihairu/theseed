# Lifecycle & Script Binding — 生命周期钩子与脚本绑定

> theseed 统一的实体生命周期钩子设计和 C++ ↔ 脚本绑定架构。
>
> 来源：BigWorld 30+ 个钩子 + PyScript 手写绑定，KBEngine 20+ 个钩子 + Python C API 手写。
> 当前实现基线以 [../0-foundation/01-mvp-architecture-baseline](../0-foundation/01-mvp-architecture-baseline.md) 为准。

---

## 1. 设计原则

```
1. 钩子只表达“什么时候发生了什么事”，不隐藏线程与时序边界
2. 钩子回调一律在 owning tick thread 上执行
3. 钩子不能绕开 tick 同步规则直接发属性包
4. 热更新只保证新入口逻辑，不能假设旧调用栈自动兼容
5. 迁移不承诺脚本栈恢复，只恢复可序列化运行时状态
```

---

## 2. Entity 生命周期钩子（实现基线）

```
                     BigWorld              KBEngine              theseed
                     ─────────             ──────────            ─────────
创建
  Base 创建          __init__()            __init__()            onCreate()
  Cell 创建          onCellCreated()       onEnterCell()         onCellCreated()
  恢复(归档/备份)     onRestore()           onRestore()           onRestore()

进入世界
  Base 进世界        onEnterWorld()        (始终在世界)           onEnterWorld()
  Cell 进世界        onEnterWorld()        onEnterWorld()        onEnterWorld()
  进入 Space         onEnterSpace()        onEnterSpace()        onEnterSpace()

客户端连接
  获得玩家控制       onBecomePlayer()      onGetCell()           onBecomePlayer()
  失去玩家控制       onLosePlayer()        onLoseCell()          onLosePlayer()
  客户端上线         onLogOn()             onClientEnabled()     onClientConnected()
  客户端断线         onClientDeath()       onClientDeath()       onClientDisconnected()
  客户端重连         (无专用)              onClientEnabled()     onClientReconnected()

Ghost 相关
  变为 Real          (始终知道)            onBecomeReal()        onBecomeReal()
  变为 Ghost         (始终知道)            onBecomeGhost()       onBecomeGhost()
  被 Witness 看到     onWitnessed()        (无)                  onWitnessed()
  不再被看到         onUnwitnessed()       (无)                  onUnwitnessed()

AOI 相关
  实体进入视野       onEnterAOI()          onEnterWorld()        onEntityEnterAOI()
  实体离开视野       onLeaveAOI()          onLeaveWorld()        onEntityLeaveAOI()

属性变更
  属性被修改         onPropertyChange()    onChanged()           onPropertyChanged()

迁移
  迁移前             onBeforeMigrate()     (无)                  onBeforeMigrate()
  迁移后             onAfterMigrate()      (无)                  onAfterMigrate()
  传送              onTeleport()          onTeleportSuccess()   onTeleport()

销毁
  离开 Space         onLeaveSpace()        onLeaveSpace()        onLeaveSpace()
  离开世界           onLeaveWorld()        onLeaveWorld()        onLeaveWorld()
  Cell 销毁          onCellDestroyed()     onLeaveCell()         onCellDestroyed()
  Base 销毁          onDestroy()           onDestroy()           onDestroy()

定时器
  定时回调           onTimer(id,userArg)   onTimer(id,userArg)   onTimer(id,userArg)

数据库
  写入数据库         onWriteToDB()         onWriteToDB()         onWriteToDB()
  从数据库加载       onReadFromDB()        (隐含在__init__)      onLoadFromDB()
```

---

## 3. 钩子边界说明

### 3.1 onPropertyChanged

```
onPropertyChanged 表示“属性在当前 tick 中被修改了”。

它不表示：
  - 该属性已经同步给客户端
  - 该属性已经同步给 ghost
  - 该属性已经持久化到数据库

同步仍然按 tick 末统一 flush 执行。
```

### 3.2 onBeforeMigrate / onAfterMigrate

```
这两个钩子只能依赖可序列化运行时状态。

不能假设：
  - 当前脚本调用栈会被搬过去
  - yield 挂起中的 continuation 会自动续跑
  - 旧版本脚本 frame 会和新位置的代码完全兼容
```

### 3.3 热更新下的钩子语义

```
受限 L2 热更新只保证“新进入的调用路径”使用新实现。

不保证：
  - 正在执行的旧脚本 frame 立刻切到新逻辑
  - 已注册回调自动获得新的闭包语义

因此：
  热更新适合替换入口逻辑，不适合修改长生命周期脚本状态机协议。
```

---

## 4. 脚本绑定层设计

### 4.1 设计原则

```
1. 零成本抽象：C++ 核心路径不经过脚本（Tick、AOI、Ghost 同步 → 纯 C++）
2. 最小暴露面：只暴露必要接口（Entity 属性、EntityCall、Controller、导航/物理、DB、定时器）
3. 类型安全：def 定义的类型自动生成强类型绑定
4. 不追求自动绑定全量 API：手写高质量核心绑定
```

### 4.2 绑定架构

```
┌────────────────────────────────────────────────────┐
│  Script Layer (Python / Lua)                        │
│  BaseEntity / CellEntity / Space                   │
│  theseed.navigation / theseed.physics / theseed.db  │
├────────────────────────────────────────────────────┤
│  Binding Layer                                      │
│  EntityBinding     → 属性/方法绑定（手写/生成混合） │
│  ControllerBinding → Controller 工厂绑定            │
│  NavigationBinding → INavigationSystem 绑定         │
│  PhysicsBinding    → IPhysicsQuery 绑定            │
│  StorageBinding    → IEntityStore / Query 绑定      │
├────────────────────────────────────────────────────┤
│  C++ Core                                           │
│  Entity / Controller / Navigation / Physics / Space │
└────────────────────────────────────────────────────┘
```

### 4.3 def 驱动的自动绑定

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

## 5. Controller 生命周期

```
                     BigWorld              KBEngine              theseed
                     ─────────             ──────────            ─────────
创建                 create()              (无显式)              onStart()
每帧更新             updatable tick        tick()                onUpdate(dt)
完成/到达            standardCallback()    (无)                  onComplete()
取消                 onCancel()            cancel()              onCancel()
实体变为 Ghost       stopReal()            (无)                  onPause()
实体变为 Real        startReal()           (无)                  onResume()
迁移序列化           writeRealToStream()   (无)                  serialize()
```

```
MVP 约束：
  - Controller 必须只依赖可序列化状态
  - 不允许把不可恢复的脚本闭包当作迁移必要状态
```
