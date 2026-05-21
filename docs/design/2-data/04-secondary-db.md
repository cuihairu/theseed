# SecondaryDB & Local Archive Store — 本地归档暂存层

> 来源：BigWorld Archiver + SecondaryDB + `consolidate_dbs / transfer_db / sync_db`。
> 这层最容易被误读成“主数据库”或“热备份”，但它实际解决的是另一类问题。

---

## 0. 设计边界

```
MVP 不承诺：
  - 内建 SecondaryDB
  - 本地归档暂存层的统一实现
  - 基于本地暂存层的快速恢复目标

Phase 2 / 3 如需补齐 BigWorld 级数据运维面：
  - 先抽象为 LocalArchiveStore
  - 再决定具体后端
  - 不把 SQLite / RocksDB 直接写死为架构前提
```

```
必须先分清三件事：
  - Primary Store：主持久化库，最终权威
  - LocalArchiveStore：本地归档暂存层，等待归并
  - EntityBackup：跨进程热备，用于更快恢复
```

---

## 1. 它到底是什么

BigWorld 的 `SecondaryDB` 不是业务主库，也不是 BaseApp 之间的内存热备。

它更接近：

```
BaseApp 本地归档暂存层
  - 接收 Base 实体的归档快照
  - 在本机快速落盘
  - 由 Archiver 周期性提交/切换代际
  - 后续再由 DBApp 统一归并回主库
```

因此它解决的问题是：

```
1. 归档写放大
   - 不让所有周期归档都直接压到远端主库

2. 停服 / 退役落盘窗口
   - 先把实体快照安全写到本机，再进入统一归并

3. 数据运维可搬运性
   - 可以 snapshot / transfer / consolidate / sync

4. 主库压力隔离
   - 把“在线写归档”和“集中归并”拆成两个阶段
```

---

## 2. 不要和另外两层混淆

| 层次 | 作用 | 典型介质 | 恢复语义 |
|------|------|----------|----------|
| Primary Store | 最终权威持久化 | MySQL / PostgreSQL / MongoDB | 进程重启后按需加载 |
| LocalArchiveStore | 本地归档暂存与待归并快照 | SQLite / 其他本地嵌入式存储 | 先落盘，再归并，不直接等于热恢复 |
| EntityBackup | 跨进程热备 | 另一台 BaseApp 内存 | 减少 RPO / RTO |

关键区分：

```
LocalArchiveStore 不是为了“替代主库”。
LocalArchiveStore 也不是为了“替代热备”。
它只是把归档路径拆成：
  运行时快照写入 → 本地暂存 → 统一归并
```

---

## 3. BigWorld 式数据流

如果 theseed 将来补这层，推荐先按 BigWorld 的职责理解，而不是先纠结具体后端。

```
Base.writeToDB / autoArchive
  → 生成实体持久化快照
  → append 到 LocalArchiveStore
  → Archiver flush / commit
  → rotate generation
  → DB 侧 Consolidator 统一归并到 Primary Store
  → 清理旧 generation
```

这条链路天然带来两个边界：

```
边界 A：运行时只负责生成快照和追加写入
边界 B：数据平台负责归并、搬运、清理、校验
```

---

## 4. 抽象设计

如果 theseed 未来需要这层，应该先抽象“本地归档暂存职责”，而不是直接把某个数据库 API 泄漏到内核。

### 4.1 本地归档暂存接口

```cpp
// storage/ILocalArchiveStore.h

struct ArchiveSnapshot {
    EntityId entityId;
    EntityTypeId entityTypeId;
    TickId tickId;
    std::vector<std::byte> payload;
};

struct ArchiveGenerationMeta {
    ArchiveGenerationId id;
    std::string location;
    TickId beginTick;
    TickId endTick;
    size_t snapshotCount;
};

class ILocalArchiveStore {
public:
    virtual ~ILocalArchiveStore() = default;

    virtual Future<void> append(const ArchiveSnapshot& snapshot) = 0;
    virtual Future<void> flush() = 0;
    virtual Future<ArchiveGenerationMeta> rotate() = 0;

    virtual Future<std::vector<ArchiveGenerationMeta>> listGenerations() = 0;
    virtual Future<void> removeGeneration(ArchiveGenerationId id) = 0;
};
```

### 4.2 归并接口

```cpp
// storage/IArchiveConsolidator.h

struct ConsolidationReport {
    size_t mergedSnapshots = 0;
    size_t skippedSnapshots = 0;
    size_t failedSnapshots = 0;
};

class IArchiveConsolidator {
public:
    virtual ~IArchiveConsolidator() = default;

    virtual Future<ConsolidationReport> consolidate(
        const ArchiveGenerationMeta& generation,
        IEntityStore& primaryStore) = 0;
};
```

设计要求：

```
1. Runtime Core 不依赖 consolidate / transfer / sync 细节
2. ILocalArchiveStore 不承担复杂查询平台职责
3. IArchiveConsolidator 不反向污染 Entity load/save 主路径
```

---

## 5. 和主持久化接口的关系

`IEntityStore` 负责主库读写，`ILocalArchiveStore` 负责本地归档暂存，两者不应混成一个“大存储接口”。

坏味道：

```
IStorageBackend:
  - load / save / remove
  - raw SQL
  - merge
  - schema migration
  - archive append
  - archive rotate
  - consolidate
```

这样会导致：

```
1. Runtime Core 被迫知道数据运维细节
2. 数据工具链反向耦合在线存储接口
3. 存储抽象退化成“能力桶”
```

因此建议的职责切分是：

```
在线主路径
  - IEntityStore

业务查询
  - IEntityQueryStore

本地归档暂存
  - ILocalArchiveStore

离线归并与运维
  - IArchiveConsolidator
  - IMergeBackend
  - ISchemaMigrator
```

---

## 6. 数据运维工具链边界

BigWorld 的数据运维面不只有“合服”。

theseed 如果要覆盖这一层，建议拆成四类工具：

| 工具类别 | 目标 | 是否属于本篇 |
|------|------|------|
| merge | 多 Realm / 多服业务数据合并 | 否 |
| snapshot / transfer | 本地归档文件快照与跨机搬运 | 是 |
| consolidate | generation 归并回主库 | 是 |
| sync / repair | Schema 或数据校验、修复、同步 | 否 |

也就是说：

```
Server Merge
  ≠ SecondaryDB transfer
  ≠ consolidation
  ≠ sync / repair
```

完整工具链边界见：

`./05-data-ops-toolchain.md`

---

## 7. 后端选择原则

这层的后端选择必须服从职责，而不是反过来用某个引擎定义职责。

### 7.1 如果目标是 BigWorld 式本地归档暂存层

更看重：

```
  - 单机单写者稳定性
  - 文件级搬运与清理简单
  - generation 轮换容易
  - 崩溃恢复与运维透明
```

这种场景下，`SQLite` 是合理候选。

### 7.2 如果目标已经升级为“本地最新状态库”

更看重：

```
  - 高频写入吞吐
  - key → latest snapshot 快速读取
  - 更长时间积压本地状态
  - 更强的本机恢复能力
```

这已经不是 BigWorld 原义上的 `SecondaryDB`，而更接近：

```
LocalStateStore
```

这种场景下，`RocksDB` 值得单独评估，但不应直接和 `SecondaryDB` 画等号。

---

## 8. theseed 的建议表述

建议把文档统一成下面这组边界：

```
MVP：
  - 主持久化 + Archiver 主路径
  - 不承诺 LocalArchiveStore

Phase 2：
  - 评估引入 LocalArchiveStore
  - 明确 snapshot / consolidate / cleanup 工具链

Phase 3：
  - 与高可用、停服、数据运维平台联动
  - 再考虑是否需要 LocalStateStore 级增强
```

---

## 9. 与两套引擎的对比

| 维度 | BigWorld | KBEngine | theseed |
|------|---------|---------|---------|
| 归档主路径 | Archiver + SecondaryDB + consolidate | writeToDB + Archiver | MVP 先主库归档 |
| 本地暂存层 | 有 | 无显式层 | Phase 2 / 3 评估 |
| 数据运维面 | transfer / consolidate / sync 较完整 | 较弱 | 需要独立设计 |
| 热备 | BackupSender | 无内建 | 另算，不和 SecondaryDB 混用 |
