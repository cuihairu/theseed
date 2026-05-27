# Fault Tolerance — 三级容错

> 来源源头：BigWorld `Reviver / BackupSender / Archiver`。
> 参考实现：KBEngine 的基础进程恢复与持久化恢复路径。
> 这是 KBEngine 差距最大的领域之一，但也是 theseed 最不应在 `MVP` 阶段过度承诺的领域。
> 当前实现基线以 [0-foundation/01-mvp-architecture-baseline](../0-foundation/01-mvp-architecture-baseline.md) 为准。

---

## 0.5 引擎实现对照与取舍

### BigWorld 是怎么实现的

```
BigWorld 的容错不是单一备份点，而是一套链条：
  - Reviver 负责进程拉起
  - BackupSender 负责状态备份链
  - Archiver 负责持久化归档
  - Manager 负责调度和恢复
```

### KBEngine 是怎么实现的

```
KBEngine 也有恢复和守护思路，
但整体更偏向基础进程恢复和数据库持久化，
没有 BigWorld 那么完整的高可用体系。
```

### 优缺点

```
BigWorld 的优点：
  - 容灾链条完整
  - 恢复边界系统化

KBEngine 的优点：
  - 简单
  - 更容易先跑起来

共同缺点：
  - 真正的 HA 复杂度高
  - 需要额外定义备份一致性和恢复顺序
```

### theseed 的取舍

```
theseed MVP 只承诺基础恢复和守护，
把完整 HA 作为单独阶段，
避免把 BigWorld 级容灾误写进当前实现边界。
```

### 为什么不能把 HA 全塞进本篇

```
Fault Tolerance 只回答：
  进程死了之后系统如何恢复

它不直接回答：
  - 热备责任链如何组织
  - backup target 如何切换
  - 谁有权裁定恢复来源

这些属于 BackupHash / HA Recovery Chain，
不能继续混在“恢复概述”里。
```

---

## 0. MVP 边界

```
MVP 先保证：
  - 进程守护和自动拉起
  - 基于数据库归档的可恢复能力
  - 基础路由修复与客户端重连

MVP 不直接承诺：
  - 完整的跨 BaseApp 内存热备
  - < 5 秒的统一恢复目标
  - 对所有实体类型都成立的低 RPO / 低 RTO
```

```
换句话说：
  ProcessSupervisor 属于 MVP
  Archiver 属于 MVP
  EntityBackup 是 Phase 2 原型能力，不应写成当前已具备的硬能力
```

---

## 1. 三级保障体系

```
第一级：ProcessSupervisor（MVP）
  来源：BigWorld Reviver
  目标：进程死亡后自动拉起、重新注册、恢复路由表

第二级：EntityBackup（Phase 2）
  来源：BigWorld BackupSender
  目标：Base 实体跨进程热备，减少恢复时间和数据丢失

第三级：Archiver（MVP）
  来源：BigWorld Archiver / KBEngine writeToDB
  目标：周期归档到持久化存储，提供可恢复基线
```

---

## 2. MVP 恢复模型

### 2.1 ProcessSupervisor

```
职责：
  - 检测 BaseApp / CellApp / Gateway 进程死亡
  - 按角色自动拉起新进程
  - 让新进程重新注册到 Control Plane
  - 触发路由表重发布
```

### 2.2 Archiver 恢复

```
MVP 下的恢复主路径：

进程死亡
  → Supervisor 拉起新进程
  → 进程重新加入集群
  → Base 实体按需从数据库归档恢复
  → 客户端重连后重新绑定会话
```

```
恢复保证：
  - 能恢复到最近一次成功归档的状态
  - 不承诺恢复未归档的全部内存态
  - 不承诺所有在途运行时消息都能恢复
```

### 2.3 RPO / RTO 说明

```
MVP 的建议表述：

RPO：
  - 由归档周期决定
  - 通常是“最多丢失一个归档周期内的未持久化状态”

RTO：
  - 由进程拉起、数据库恢复、客户端重连共同决定
  - 不写死全局秒级指标
```

```
原因：
  在没有跨进程热备、WAL 和统一 in-flight drain 机制之前，
  写死 “< 5 秒恢复、最多丢 5 秒” 是愿景，不是设计约束。
```

---

## 3. BigWorld 的 SecondaryDB 不等于 EntityBackup

BigWorld 的实现里，`Archiver` 经常和 `SecondaryDB` 一起出现。

但要注意：

```
SecondaryDB 解决的是：
  - 本地归档暂存
  - 停服 / 退役窗口落盘
  - 后续统一归并

EntityBackup 解决的是：
  - 跨 BaseApp 内存热备
  - 更低 RPO / 更快恢复
```

因此在设计上必须拆开：

```
Archiver / LocalArchiveStore
  ≠ EntityBackup
```

反过来说：

```
只要某个方案的核心目标是：
  - 降低 RPO / RTO
  - 在主实例未落库前保住内存态

那它就在讨论热备，
而不是归档暂存层。
```

对 theseed 的约束是：

```
MVP：
  - 只承诺“主库归档 + 可恢复”
  - 不把 SecondaryDB / LocalArchiveStore 写成当前已具备能力

Phase 2 / 3：
  - 如需补齐 BigWorld 级数据运维面，再引入 LocalArchiveStore
  - 但这仍然不能替代 EntityBackup
```

---

## 4. EntityBackup — Phase 2 能力

```
问题：BaseApp 宕机时，上面所有实体的未写库数据可能丢失。

BigWorld 的做法：
  - BackupSender 把 Base 实体数据发送到另一个 BaseApp
  - 备份在内存中
  - 宕机时从备份 BaseApp 恢复

theseed 的方向：
  - 引入 BackupHash + 跨 BaseApp 内存热备
  - 但该能力属于 Phase 2，不是 MVP 必达项
```

### 4.1 预留接口

```cpp
// runtime/EntityBackup.h

class EntityBackupManager {
public:
    void setBackupPolicy(const BackupPolicy& policy);
    void registerBackupTarget(EntityId id, ComponentId backupBaseApp);
    void backupEntity(Entity* entity);
    void restoreFromBackup(ComponentId deadBaseApp,
                           ComponentId newBaseApp);

    bool hasBackup(EntityId id) const;
    ComponentId getBackupLocation(EntityId id) const;
};

struct BackupPolicy {
    Duration backup_interval = 5s;
    int max_backups_per_entity = 3;
    bool compress_backup = true;
    size_t max_backup_memory_mb = 512;
};
```

### 4.2 Phase 2 恢复流程

```
BaseApp_A 宕机
  → Control Plane 拉起 BaseApp_A'
  → 查询 BackupHash
  → 从备份 BaseApp 拉取 Base 实体快照
  → 恢复属性 / 路由 / 会话
  → 通知客户端重连
```

```
前提：
  - 备份复制链路已经稳定
  - Base / Cell 路由切换可观测
  - 会话恢复和异步回调 epoch 校验已完善
```

---

## 5. LocalArchiveStore — Phase 2 / 3 数据运维增强

如果 theseed 未来要补齐 BigWorld 级数据运维面，应新增一层：

```
LocalArchiveStore
  - 本地归档暂存
  - generation rotate
  - snapshot / transfer / consolidate
```

但文档边界必须写清：

```
它不是：
  - 主数据库
  - 热备内存副本
  - Runtime Core 的查询平台
```

建议职责：

```
Runtime：
  - 生成归档快照
  - append / flush / rotate

Data Ops：
  - consolidate generation
  - transfer / snapshot / cleanup
  - 校验与同步
```

相关设计见：

`../4-data-and-ops/03-local-archive-and-secondary-db.md`

热备链与恢复权威边界见：

`./02-backup-hash-and-ha.md`

---

## 6. 分阶段目标

```
Phase A（MVP）
  - Supervisor 自动拉起
  - Archiver 可恢复
  - 客户端可重连

Phase B
  - Base 热备原型
  - 路由修复自动化
  - 恢复流程可观测

Phase C
  - 稳定的跨 BaseApp 内存热备
  - 更低 RPO / RTO
  - 备份一致性监控
```

---

## 7. 与两套引擎的对比

| 维度 | BigWorld | KBEngine | theseed |
|------|---------|---------|---------|
| 进程守护 | Reviver 自动拉起 | 无内建 | MVP 先做 Agent + Control Plane |
| 实体热备 | BackupSender 跨进程 | 无（只有 DB 归档） | Phase 2 目标 |
| 恢复主路径 | 内存热备 + 归档 | 数据库恢复 | MVP 先走数据库恢复 |
| 数据丢失 | < 1 个备份周期 | < 1 个 Archiver 周期 | MVP 由归档周期决定 |
| 归档 | Archiver + SecondaryDB + consolidate | writeToDB + Archiver | MVP 主库归档，Phase 2 再评估 LocalArchiveStore |
