# Entity Migration — 实体迁移

> 实体迁移是 theseed 分布式架构的核心能力：实体可以在不同 CellApp 之间安全迁移。
>
> 来源：KBEngine teleport + Ghost 路由窗口，BigWorld Offload 机制。

---

## 1. 迁移场景

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

---

## 2. 迁移流程

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

---

## 3. 迁移窗口期的消息处理

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
