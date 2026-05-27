# Entity Migration — 实体迁移

> 实体迁移是 theseed 分布式架构的核心能力：实体可以在不同 CellApp 之间安全迁移。
>
> 来源源头：BigWorld Offload / Cell 迁移 / 退役迁出机制。
> 参考实现：KBEngine teleport + Ghost 路由窗口。
> 当前实现基线以 [0-foundation/01-mvp-architecture-baseline](../0-foundation/01-mvp-architecture-baseline.md) 为准。

---

## 0.5 引擎实现对照与取舍

### BigWorld 是怎么实现的

```
BigWorld 的迁移不是单纯的 teleport：
  - 有 offload / retire / controlled shutdown 语义
  - 有跨 Cell 迁移窗口
  - 有更完整的负载均衡和退役链条
```

### KBEngine 是怎么实现的

```
KBEngine 更偏向 teleport 和 ghost 路由窗口：
  - 迁移路径更直接
  - 系统级退役链条更轻
  - 适合先把实体可迁移跑通
```

### 优缺点

```
BigWorld 的优点：
  - 迁移和退役体系完整
  - 能支撑更复杂的集群调度

KBEngine 的优点：
  - 简单
  - 容易先落地

共同缺点：
  - 迁移快照和窗口期控制都很敏感
  - 一旦和脚本 I/O、协程绑死，复杂度会迅速上升
```

### theseed 的取舍

```
theseed 先实现单 Realm 内迁移和跨 Space 传送，
把脚本调用栈和外部 I/O 明确排除在快照之外，
避免把 BigWorld 的 HA 复杂度一次性吞进 MVP。
```

---

## 0. MVP 边界

```
MVP 只支持：
  - 单 Realm 内 Cell 迁移
  - 跨 Space 传送

MVP 不支持：
  - 跨机房 / 跨 Realm 实体迁移
  - 脚本调用栈迁移
  - 挂起协程 / Future continuation 迁移
  - 外部 I/O 上下文迁移
```

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

场景 4: 跨服（远期）
  实体从一个 Realm 迁移到另一个 Realm
  不属于 MVP 范围
```

---

## 2. 迁移流程

```
Phase 1: Prepare
  ├─ 源 CellApp: 冻结实体收件箱
  ├─ 标记 entity epoch / migration token
  ├─ 设置 GhostManager 路由（转发窗口期的消息）
  └─ 通知目标 CellApp 准备接收

Phase 2: Serialize
  ├─ 序列化实体状态：
  │   - PropertyBlock（持久化 + 运行时必要属性）
  │   - 位置 / 朝向 / Space 信息
  │   - 定时器列表
  │   - 控制器状态
  │   - Witness 基础状态
  └─ 发送到目标 CellApp

  不进入迁移快照：
    - Python / Lua 调用栈
    - yield 挂起中的 continuation
    - 正在执行的 HTTP / DB / 跨服请求上下文

Phase 3: Restore
  ├─ 目标 CellApp: 创建新实体
  ├─ 反序列化属性
  ├─ 恢复定时器
  ├─ 加入 Space / CoordinateSystem
  ├─ 恢复 Witness（如果有客户端）
  └─ 建立新的 entity epoch

Phase 4: Route Update
  ├─ 更新 EntityCall 路由：旧 CellApp → 新 CellApp
  ├─ 通知 BaseApp 更新 cellEntityCall 目标
  ├─ GhostManager 路由窗口期：将运行时消息转发到新位置
  └─ 通知已知持有方更新地址

Phase 5: Cleanup
  ├─ 旧 CellApp: 销毁旧实体（callScript=false）
  ├─ 清理 GhostManager 路由
  └─ 丢弃旧 epoch 的迟到回调
```

---

## 3. 迁移窗口期的消息处理

```
问题：迁移过程中，运行时消息可能发到旧的 CellApp

KBEngine 的解决方案（来自源码注释）：
  "如果期间有base的消息发送过来，entity的ghost机制能够转到real上去"

theseed 的设计：
  GhostManager.setRoute(target, ttl=10s)
  → 旧 CellApp 在路由窗口期内，把运行时消息转发到新位置
  → 路由过期后，如果还有消息发来，说明路由表没更新完，记录告警

覆盖范围：
  - EntityCall
  - Ghost / Witness 同步
  - 迁移窗口内的运行时控制消息

不覆盖：
  - 已经发出的外部 HTTP 请求
  - 已提交的 DB 异步请求
  - 跨 Realm 异步任务的远端执行上下文

因此：
  迁移窗口机制的目标是“运行时消息无感切换”，
  不是对所有异步副作用承诺绝对零丢失。
```
