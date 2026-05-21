# Fault Tolerance — 三级容错

> 来源：BigWorld Reviver + BackupSender + Archiver。
> 这是 KBEngine 差距最大的领域（Ch23："这是两套项目差距最大的领域"）。
> theseed 必须补上。

---

## 1. 三级保障体系

```
第一级：ProcessSupervisor（进程守护）
  来源：BigWorld Reviver
  改进：theseed 用 Agent + Control Plane 替代 bwmachined

第二级：EntityBackup（跨进程热备）
  来源：BigWorld BackupSender
  KBEngine 缺失：只有 Backuper/writeBackupData，不是灾备恢复链
  theseed 改进：引入 BackupHash + 跨 BaseApp 内存热备

第三级：Archiver（周期归档）
  来源：BigWorld Archiver / KBEngine writeToDB
  改进：theseed 用 IStorageBackend 抽象 + 多后端支持
```

---

## 2. EntityBackup — 跨进程热备

```
问题：BaseApp 宕机时，上面所有实体的未写库数据全部丢失。

KBEngine 的现状：
  - Backuper 周期调用 writeBackupData
  - 但备份数据存在同一个 DBMgr，不是跨进程热备
  - 恢复时从数据库加载（慢，可能丢最近的数据）

BigWorld 的做法：
  - BackupSender 把 Base 实体数据发送到另一个 BaseApp
  - 备份在内存中（快）
  - BaseApp 宕机时从备份 BaseApp 恢复（秒级）

theseed 的设计：
```

### 2.1 核心接口

```cpp
// runtime/EntityBackup.h

class EntityBackupManager {
public:
    // 配置备份策略
    void setBackupPolicy(const BackupPolicy& policy);

    // 注册 Base 实体的备份目标
    // BackupHash: entityId → backup BaseApp 的一致性哈希映射
    void registerBackupTarget(EntityId id, ComponentId backupBaseApp);

    // 触发备份（每个 Archiver 周期调用）
    void backupEntity(Entity* entity);

    // BaseApp 宕机时恢复
    void restoreFromBackup(ComponentId deadBaseApp,
                           ComponentId newBaseApp);

    // 查询
    bool hasBackup(EntityId id) const;
    ComponentId getBackupLocation(EntityId id) const;
};

struct BackupPolicy {
    Duration backup_interval = 5s;     // 备份间隔
    int max_backups_per_entity = 3;    // 每实体保留的备份版本数
    bool compress_backup = true;       // 是否压缩备份数据
    size_t max_backup_memory_mb = 512; // 备份 BaseApp 的最大内存使用
};
```

### 2.2 备份恢复流程

```
BaseApp_A 宕机 → 检测到死亡
  │
  ├─ 1. ControlPlane 启动新 BaseApp_A'
  │
  ├─ 2. 查询 BackupHash：BaseApp_A 上的实体 → 备份在哪些 BaseApp
  │     例: Entity 10042 → 备份在 BaseApp_B
  │         Entity 10089 → 备份在 BaseApp_C
  │
  ├─ 3. BaseApp_A' 从各备份 BaseApp 拉取实体数据
  │     BaseApp_A' → BaseApp_B: restoreBackup(entityIds)
  │     BaseApp_A' → BaseApp_C: restoreBackup(entityIds)
  │
  ├─ 4. BaseApp_A' 反序列化实体，恢复到内存
  │     - 恢复属性
  │     - 恢复定时器
  │     - 重新建立 EntityCall 路由
  │     - 如果实体有 Cell 部分，通知 CellApp 更新 baseEntityCall
  │
  ├─ 5. 通知客户端重连到 BaseApp_A'
  │     - 客户端检测断线
  │     - LoginApp 告知新 BaseApp 地址
  │     - 客户端用 session token 恢复（不需要重新登录）
  │
  └─ 6. 更新路由表
        - BaseAppMgr 更新全局路由
        - 所有 EntityCall 持有者更新目标地址

恢复时间目标：< 5 秒（内存热备，不经过数据库）
数据丢失：最多一个 backup interval（默认 5s）
```

---

## 3. 与两套引擎的对比

| 维度 | BigWorld | KBEngine | theseed |
|------|---------|---------|---------|
| 进程守护 | Reviver 自动拉起 | 无内建 | Agent + Control Plane |
| 实体热备 | BackupSender 跨进程 | 无（只有 DB 归档） | BackupHash + 跨 BaseApp |
| 恢复速度 | 秒级（内存） | 分钟级（数据库） | 秒级（内存） |
| 数据丢失 | < 1 个备份周期 | < 1 个 Archiver 周期 | < 1 个备份周期 |
| 归档 | Archiver + SecondaryDB | writeToDB + Archiver | IStorageBackend 多后端 |
