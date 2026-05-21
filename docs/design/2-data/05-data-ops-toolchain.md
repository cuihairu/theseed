# Data Ops Toolchain — Snapshot、Transfer、Consolidate、Sync 与 Repair

> 来源：BigWorld `transfer_db / consolidate_dbs / sync_db / DBApp consolidateData`。
> 这篇承接的是“数据运维工具链”，不是 Runtime Core，也不是单纯合服。

---

## 0. 设计边界

```
本篇负责：
  - 本地归档 generation 的 snapshot / transfer / consolidate
  - 主库与暂存层之间的数据运维流程
  - schema / consistency 的 sync 与 repair 边界

本篇不负责：
  - Entity load / save 主路径
  - 合服业务冲突解决
  - Base 实体热备路由
```

和其他文档的关系：

```
02-persistence
  - 在线主持久化接口

03-server-merge
  - 多服业务数据合并

04-secondary-db
  - LocalArchiveStore 是什么

本篇
  - 运维如何搬运、归并、校验这些数据
```

---

## 1. 为什么必须独立成篇

BigWorld 的数据层差异，不只是“有 SecondaryDB”。

真正的系统面在于它有配套的运维链：

```
1. 在线归档先落到本地暂存
2. 暂存可 snapshot / transfer
3. DBApp 可在启动或停服阶段做 consolidate
4. 还存在 sync / repair 一类库外修复能力
```

如果这些能力继续只散落在：

```
Persistence
SecondaryDB
Server Merge
```

会出现两个问题：

```
1. 在线主路径和离线工具链边界混乱
2. “合服”被误当成全部数据运维能力
```

---

## 2. 数据运维面的四条线

theseed 应明确把数据运维面拆成四条线：

| 线 | 目标 | 典型触发 |
|------|------|----------|
| snapshot / transfer | 搬运暂存数据或主库快照 | 迁机、备份、停服 |
| consolidate | 把 generation 归并回 Primary Store | 启动、停服、定时窗口 |
| sync | 做 schema / metadata 对齐 | 升级、环境修复 |
| repair | 做 consistency 检查与修复 | 故障后、离线巡检 |

不要再把它们统称成“数据库工具”。

---

## 3. 核心对象模型

### 3.1 Primary Store

```
最终权威
```

### 3.2 Archive Generation

```
某次 rotate 形成的一批本地归档快照集合
```

### 3.3 Snapshot Package

```
可搬运、可校验的快照包
```

### 3.4 Consolidation Job

```
把 generation 归并回主库的离线或半在线作业
```

### 3.5 Sync / Repair Job

```
以 schema、索引、引用关系、一致性校验为目标的作业
```

---

## 4. 典型数据流

### 4.1 在线归档增强路径

```
Runtime / Archiver
  → append snapshot
  → LocalArchiveStore.flush
  → rotate generation
  → Data Ops 接管
```

### 4.2 搬运路径

```
generation
  → snapshot package
  → checksum / manifest
  → transfer to remote node or object storage
```

### 4.3 归并路径

```
generation
  → validate manifest
  → consolidate into Primary Store
  → mark committed
  → cleanup old generation
```

### 4.4 修复路径

```
schema diff / consistency scan
  → report
  → operator review
  → repair plan
  → apply
```

---

## 5. Snapshot

### 5.1 职责

```
snapshot 负责：
  - 把一批 generation 或主库导出为可搬运包
  - 生成 manifest、checksum、版本信息
  - 为 transfer / restore / audit 提供输入
```

### 5.2 不要写成什么

```
snapshot 不是：
  - 运行时实时查询副本
  - 热备恢复链
  - 合服导入格式
```

### 5.3 设计要求

```
1. snapshot 包必须可校验
2. snapshot 版本必须显式记录 schema version
3. snapshot 不能隐式写回主库
```

---

## 6. Transfer

BigWorld 的 `transfer_db` 说明传输本身就是一个单独工具面。

### 6.1 职责

```
transfer 负责：
  - 把 snapshot / generation 搬运到目标节点
  - 控制带宽、重试、断点重传
  - 保证目标侧能验证完整性
```

### 6.2 设计要求

```
1. transfer 不直接修改 Primary Store
2. transfer 必须有 manifest 校验
3. transfer 成功不等于 consolidate 成功
```

---

## 7. Consolidate

BigWorld 的 `DBApp::consolidateData()` 说明归并是独立阶段，而不是归档 append 的顺带动作。

### 7.1 职责

```
consolidate 负责：
  - 读取一个或多个 generation
  - 去重、顺序化、合并到 Primary Store
  - 标记成功 / 失败 / 可重试状态
```

### 7.2 语义要求

```
1. consolidate 是“主库写入阶段”
2. consolidate 必须可重试
3. consolidate 失败不能默认删除 generation
4. consolidate 需要 operator 可见状态
```

### 7.3 触发时机

```
启动阶段：
  - 处理遗留 generation

停服阶段：
  - 尽量完成最后一轮归并

运维窗口：
  - 周期性归并与清理
```

---

## 8. Sync

`sync` 不应再被混进 `merge` 或 `migration`。

### 8.1 职责

```
sync 负责：
  - schema / index / metadata 对齐
  - 环境间配置化同步
  - 数据面只做“格式 / 元信息一致性”层面的修正
```

### 8.2 典型场景

```
1. 升级后 schema drift
2. 某环境缺失索引或约束
3. generation manifest 与主库 metadata 不一致
```

---

## 9. Repair

repair 的定位必须比 sync 更保守。

### 9.1 职责

```
repair 负责：
  - 发现并修复坏数据
  - 处理 orphan reference / broken unique index / partial write 残留
  - 输出修复报告和审计记录
```

### 9.2 设计约束

```
1. repair 默认先 report，再 apply
2. repair 必须支持 dry-run
3. repair 不允许伪装成在线主路径能力
```

---

## 10. theseed 的工具链模型

### 10.1 CLI 建议

```bash
# snapshot
theseed-data snapshot create --source local-archive --generation latest --output snapshot.tar.zst
theseed-data snapshot verify --input snapshot.tar.zst

# transfer
theseed-data transfer push --input snapshot.tar.zst --target 10.0.0.12:/data/inbox
theseed-data transfer pull --source 10.0.0.12:/data/inbox/snapshot.tar.zst

# consolidate
theseed-data consolidate run --generation 2026-05-21T020000Z
theseed-data consolidate status --job 9f4d8e

# sync / repair
theseed-data sync plan --schema defs/current
theseed-data repair scan --check references,unique,json
theseed-data repair apply --plan repair-plan.json
```

### 10.2 职责分层

```
Runtime
  - 只负责生成归档快照

Data Ops Agent / CLI
  - snapshot / transfer / consolidate / sync / repair

Ops Control Plane
  - 只负责触发、查看状态、审计
```

---

## 11. 核心接口建议

### 11.1 Snapshot

```cpp
// dataops/IArchiveSnapshotService.h

struct SnapshotManifest {
    std::string snapshotId;
    std::string schemaVersion;
    std::vector<ArchiveGenerationId> generations;
    uint64 createdAtUnixMs = 0;
};

class IArchiveSnapshotService {
public:
    virtual ~IArchiveSnapshotService() = default;

    virtual Future<SnapshotManifest> createFromGeneration(
        ArchiveGenerationId id,
        const std::string& outputPath) = 0;

    virtual Future<void> verify(const std::string& inputPath) = 0;
};
```

### 11.2 Transfer

```cpp
// dataops/IArchiveTransferService.h

class IArchiveTransferService {
public:
    virtual ~IArchiveTransferService() = default;

    virtual Future<void> push(const std::string& packagePath,
                              const std::string& targetUri) = 0;
    virtual Future<void> pull(const std::string& sourceUri,
                              const std::string& outputDir) = 0;
};
```

### 11.3 Consolidate

```cpp
// dataops/IConsolidationService.h

enum class ConsolidationState {
    Pending,
    Running,
    Succeeded,
    Failed
};

struct ConsolidationJobStatus {
    std::string jobId;
    ArchiveGenerationId generationId;
    ConsolidationState state = ConsolidationState::Pending;
};

class IConsolidationService {
public:
    virtual ~IConsolidationService() = default;

    virtual Future<ConsolidationJobStatus> run(
        ArchiveGenerationId generationId) = 0;
    virtual Future<ConsolidationJobStatus> status(
        const std::string& jobId) const = 0;
};
```

### 11.4 Sync / Repair

```cpp
// dataops/IDataRepairService.h

struct RepairPlan {
    std::string planId;
    std::vector<std::string> checks;
    bool dryRun = true;
};

class IDataRepairService {
public:
    virtual ~IDataRepairService() = default;

    virtual Future<RepairPlan> scan(const std::vector<std::string>& checks) = 0;
    virtual Future<void> apply(const RepairPlan& plan) = 0;
};
```

---

## 12. 与合服的关系

必须持续强调：

```
merge
  是多服业务数据整合

snapshot / transfer / consolidate / sync / repair
  是单服或单集群的数据运维能力
```

两者都属于数据工具链，但不是一条线。

---

## 13. 分阶段边界

```
MVP：
  - 先只保留主持久化与基础 backup / restore 方案
  - 不承诺完整 Data Ops Toolchain

Phase 2：
  - LocalArchiveStore + snapshot / consolidate
  - transfer 基础能力
  - sync 的最小 schema 对齐

Phase 3：
  - repair / consistency 平台
  - 与停服、迁机、跨环境运维流程联动
  - 更丰富的对象存储与远程搬运后端
```

---

## 14. 与 BigWorld / KBEngine / theseed 的对比

| 维度 | BigWorld | KBEngine | theseed |
|------|---------|---------|---------|
| SecondaryDB 搬运 | 有 `transfer_db` | 基本无 | Phase 2 目标 |
| consolidation | 有 | 较弱 | Phase 2 目标 |
| sync / repair | 有工具链雏形 | 较弱 | 需单独设计 |
| merge 与 data ops 分层 | 事实存在但文档不统一 | 弱 | 显式拆分 |
