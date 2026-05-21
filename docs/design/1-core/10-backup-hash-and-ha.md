# Backup Hash & HA Recovery Chain — 热备链、恢复权威与切换边界

> 来源：BigWorld `BackupHash / BackupHashChain / BackupSender / BaseAppMgr::useNewBackupHash / requestBackupHashChain`。
> 这篇不是在重复 `EntityBackup`，而是把 BigWorld 真正成体系的热备链路单独立住。

---

## 0. 设计边界

```
本篇负责：
  - BackupHash / BackupHashChain 的职责边界
  - primary / backup ownership
  - newBackupHash 切换流程
  - retire / crash 时的恢复权威与链路调整

本篇不负责：
  - 主数据库归档接口
  - LocalArchiveStore 的文件组织
  - 具体序列化格式与压缩算法
```

它和其他文档的关系：

```
09-fault-tolerance
  - 说明 why 需要多级容错

04-secondary-db
  - 说明本地归档暂存层是什么

本篇
  - 说明 BigWorld 级热备链到底怎么成立
```

---

## 1. 为什么必须单独成篇

BigWorld 的高可用差异，不在于“多一个备份副本”。

真正的差异在于它把下面几件事做成了一个系统：

```
1. 每个 Base 实体都能映射到备份落点
2. 备份落点变更不是瞬时替换，而是有切换期
3. retire 期间 backup hash 必须冻结
4. BaseApp 死亡后，谁负责恢复、恢复到哪，都是链路内可推导的
5. BaseAppMgr 能把 backup hash chain 作为集群级权威分发
```

如果不把这层写出来，`EntityBackup` 会被误解成：

```
“把实体快照丢给另一台 BaseApp 就行”
```

这和 BigWorld 的实现不是一个层级。

---

## 2. 它到底是什么

### 2.1 BackupHash

`BackupHash` 解决的是：

```
EntityId
  → backup BaseApp location
```

也就是说，它描述的是“某个 Base 实体的热备副本应该落到哪里”。

### 2.2 BackupHashChain

`BackupHashChain` 解决的是：

```
当某个 BaseApp 死亡时：
  - 受影响的备份责任如何重排
  - 哪些 BaseApp 正在为哪些 BaseApp 持有备份
  - 恢复后的新实例应接回哪条链
```

所以它不是简单的哈希表，而是：

```
集群级备份责任链
```

### 2.3 newBackupHash

BigWorld 里 `newBackupHash` 的存在说明了一件关键事实：

```
备份落点变更不是原子替换。
必须先 priming 新链，再 ack 切换。
```

这也是 theseed 文档此前还没单独讲清的点。

---

## 3. 核心概念

### 3.1 Primary Ownership

```
Primary
  - 实体当前运行时权威
  - 负责对外服务
  - 负责生成备份快照
```

### 3.2 Backup Ownership

```
Backup
  - 持有热备副本
  - 不对外成为实体主权威
  - 仅在主实例失效后参与恢复
```

### 3.3 Restore Authority

```
Restore Authority
  - 宕机后谁有权发起恢复
  - 恢复后实体应绑定到哪个新 primary
  - 谁来裁定 backup snapshot 是否可用
```

在 theseed 里，这个权威不应散落在各个 BaseApp 本地判断里，而应由：

```
Cluster Control Plane / HA Coordinator
```

统一给出。

---

## 4. BigWorld 式链路模型

推荐 theseed 在文档层明确下面这条模型：

```
Entity
  → primary BaseApp
  → backup BaseApp (via BackupHash)
  → cluster-level adjustment (via BackupHashChain)
```

在运行中会出现三种 hash 视图：

```
1. active hash
   - 当前已生效的备份落点

2. staging hash
   - 正在 priming 的新备份落点

3. chain authority
   - 集群级的备份责任链权威
```

对应 BigWorld 的语义可以抽象成：

```
backupHash
newBackupHash
BackupHashChain
```

---

## 5. 切换不是瞬时操作

BigWorld 的 `useNewBackupHash` / `ackNewBackupHash` 说明切换至少包含两个阶段：

### 5.1 Priming

```
旧链仍然有效
新链开始接收初始化备份
```

目的：

```
避免直接切换后新备份侧还没有完整实体快照
```

### 5.2 Ack & Promote

```
当新链完成 priming：
  - BaseApp ack 新 hash
  - BaseAppMgr / HA Coordinator 提升新 hash 为 active
  - 旧 hash 退役
```

这意味着 theseed 不应把切换写成：

```
rebalance 之后直接替换 backup target
```

正确表述应是：

```
prepare
  → prime
  → ack
  → promote
```

---

## 6. retire 时的关键约束

BigWorld 在 retire 期间有一个很强的约束：

```
retire 中的 BaseApp 必须保持 backup hash 稳定
```

原因不是实现细节，而是系统正确性：

```
如果 offloading 途中 backup hash 继续变化，
那么某个实体一旦在迁移窗口宕机，
系统可能无法判定它应从哪里恢复、恢复后又该迁到哪里。
```

因此 theseed 应明确：

```
Draining / Retiring 状态下：
  - primary route 可以迁移
  - backup hash 不应随意改写
  - 如需切换，必须等本轮 offload 完成或重新建链
```

---

## 7. crash 时的恢复权威

### 7.1 恢复流程

推荐 theseed 将热备恢复主路径写成：

```
BaseApp_A crash
  → Supervisor / Control Plane 发现失效
  → HA Coordinator 查询 chain authority
  → 确认 A 上实体对应的 backup owners
  → 拉起 BaseApp_A' 或选定新 primary
  → 从 backup owners 恢复实体快照
  → 修复路由 / 会话 / epoch
  → 发布新的 backup chain
```

### 7.2 权威要求

这里必须强调三点：

```
1. 恢复入口必须单点裁定
   - 防止多个 backup 同时自发“接管”

2. 恢复目标必须明确
   - 恢复到原位新实例，还是恢复到其他可承载实例

3. 恢复后的 backup 关系必须重建
   - 恢复成功不等于 HA 闭环完成
```

---

## 8. 和 EntityBackup / LocalArchiveStore 的关系

三者必须严格区分：

| 层次 | 作用 | 典型介质 | 语义 |
|------|------|----------|------|
| EntityBackup | 单个实体热备快照 | 另一台 BaseApp 内存 | 更低 RPO / 更快恢复 |
| BackupHash / Chain | 备份责任路由与恢复权威 | 集群控制面状态 | 决定备份该发给谁、死后从谁恢复 |
| LocalArchiveStore | 本地归档暂存 | SQLite / 其他嵌入式存储 | 停服落盘、归并、搬运 |

所以关系是：

```
EntityBackup
  依赖 BackupHash / Chain 才能形成系统化 HA

LocalArchiveStore
  不提供 backup routing authority
```

---

## 9. theseed 的抽象建议

### 9.1 HA 协调接口

```cpp
// ha/IBackupTopologyCoordinator.h

struct BackupRoute {
    EntityId entityId;
    ProcessId primary;
    ProcessId backup;
    uint64 epoch = 0;
};

struct BackupTopologyVersion {
    uint64 version = 0;
    uint64 epoch = 0;
};

class IBackupTopologyCoordinator {
public:
    virtual ~IBackupTopologyCoordinator() = default;

    virtual BackupRoute routeFor(EntityId entityId) const = 0;
    virtual BackupTopologyVersion version() const = 0;

    virtual Future<void> beginRebuild(ProcessId reasonProcess) = 0;
    virtual Future<void> ackPrimed(
        ProcessId processId,
        BackupTopologyVersion version) = 0;
};
```

### 9.2 热备管理接口

```cpp
// ha/IEntityBackupReplicator.h

struct BackupSnapshot {
    EntityId entityId;
    EntityTypeId entityTypeId;
    TickId tickId;
    uint64 topologyEpoch = 0;
    std::vector<std::byte> payload;
};

class IEntityBackupReplicator {
public:
    virtual ~IEntityBackupReplicator() = default;

    virtual Future<void> replicate(const BackupSnapshot& snapshot) = 0;
    virtual Future<void> prime(BackupTopologyVersion version) = 0;
    virtual Future<void> restorePrimary(
        ProcessId deadPrimary,
        ProcessId newPrimary) = 0;
};
```

设计要求：

```
1. route authority 和 snapshot transport 分层
2. epoch/version 必须显式可见
3. restorePrimary 不等于“随便找个备份读出来”
```

---

## 10. 状态机建议

### 10.1 拓扑状态

```
Stable
  → Rebuilding
  → Priming
  → Promoting
  → Stable
```

异常分支：

```
Priming → Aborted
Promoting → Aborted
```

### 10.2 进程侧状态

```
Running
  → Draining
  → Retiring
  → Stopped

Running
  → Failed
  → Recovering
  → Running
```

要求：

```
1. Retiring 时不能接受不受控的 backup topology rebuild
2. Recovering 完成后必须重新接入 backup chain
3. topology version 必须随恢复事件一起记录
```

---

## 11. 分阶段边界

```
MVP：
  - 不承诺 BackupHash / BackupHashChain
  - 只保留抽象名词和职责边界
  - 恢复仍以数据库归档为主

Phase 2：
  - 引入 BackupTopologyCoordinator
  - 支持单副本 Base 热备
  - 支持 prepare → prime → ack → promote 切换链路

Phase 3：
  - 更稳定的恢复权威裁定
  - 更细粒度的 topology health / skew 检查
  - 与 rolling update / retire / data ops 平台联动
```

---

## 12. 与 BigWorld / KBEngine / theseed 的对比

| 维度 | BigWorld | KBEngine | theseed |
|------|---------|---------|---------|
| BackupHash | 有 | 无 | Phase 2 目标 |
| BackupHashChain | 有 | 无 | Phase 2 目标 |
| newBackupHash 切换期 | 有 | 无 | 显式建模 |
| retire 时冻结备份链 | 有 | 无 | 明确写入边界 |
| 热备恢复权威 | 集群级协调 | 基本无 | 交给 HA Coordinator |
