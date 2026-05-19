# Developer Experience & Infrastructure Design

> 本文是 theseed 引擎的"开发体验层"设计，覆盖 Debug、Profile、运维自动化、热更新、跨服、数据定义六个方向。
> 设计目标：让开发者在**本地单机调试**和**线上千台集群运维**之间，获得一致的心智模型。

---

## 1. Debug 体系

### 1.1 设计目标

- 单机开发时：IDE 断点、单步、变量查看，和调试普通 C++/Python 程序无异
- 集群运行时：能attach到任意进程，不重启、不影响线上流量
- 跨进程调用链：EntityCall 从 A 进程到 B 进程时，trace ID 自动传播

### 1.2 架构分层

```
┌──────────────────────────────────────────────┐
│  IDE Integration (VS Code / JetBrains)       │  ← DAP 协议
├──────────────────────────────────────────────┤
│  Debug Bridge (统一接入层)                     │
│  - DebugSessionManager                       │
│  - BreakpointRegistry                        │
│  - VariableInspector                         │
├──────────────────────────────────────────────┤
│  Language Debug Adapter                      │
│  ├─ C++   → LLDB / GDB via MI protocol      │
│  ├─ Python → pydevd / debugpy                │
│  └─ Lua   → custom DAP adapter               │
├──────────────────────────────────────────────┤
│  Engine Debug Hooks                          │
│  - Entity lifecycle breakpoints              │
│  - Message receive/send breakpoints          │
│  - Timer callback breakpoints                │
│  - AOI enter/leave breakpoints               │
└──────────────────────────────────────────────┘
```

### 1.3 核心接口

```cpp
// debug/IDebugProvider.h

class IDebugProvider {
public:
    virtual ~IDebugProvider() = default;

    // 生命周期钩子
    virtual void onEntityCreated(EntityId id, const std::string& entityType) = 0;
    virtual void onEntityDestroyed(EntityId id) = 0;

    // 消息钩子
    virtual void onMessageSent(EntityId from, EntityId to, const std::string& method) = 0;
    virtual void onMessageReceived(EntityId target, const std::string& method) = 0;

    // 变量查看
    virtual std::string inspectEntity(EntityId id) = 0;
    virtual std::string inspectProperty(EntityId id, const std::string& propName) = 0;

    // 条件断点
    virtual void addConditionalBreakpoint(const std::string& condition,
                                          BreakpointCallback cb) = 0;
};
```

### 1.4 与 KBEngine 的对比

| 能力 | KBEngine | theseed |
|------|---------|---------|
| 日志 | `LOG_MSG` 宏，grep 友好 | 结构化日志 + ELK 友好 |
| 断点 | 无内建机制 | DAP 协议，IDE 原生支持 |
| 变量查看 | Telnet `pyExec` 命令 | IDE Variables Panel |
| 远程调试 | 无 | 可 attach 任意进程 |
| 调用链追踪 | 无 | TraceID 跨进程传播 |
| Entity 快照 | 无 | 内存快照 + diff |

### 1.5 与前序层的关系

```
ITransport 发消息 → onMessageSent/onMessageReceived → DebugProvider
IScriptBackend 执行脚本 → 脚本断点触发 → DebugProvider
IEntity 生命周期 → onEntityCreated/onEntityDestroyed → DebugProvider
```

---

## 2. Performance Profiling

### 2.1 设计目标

- **零成本抽象**：release 构建下所有 probe 编译为空操作
- **tick 级粒度**：能回答"这个 tick 花了多少时间在哪个阶段"
- **火焰图就绪**：采样数据直接输出 folded stack 格式
- **热路径识别**：自动标记超过阈值的 tick

### 2.2 架构

```
┌──────────────────────────────────────────────┐
│  Profiling Dashboard (Web UI)                │
├──────────────────────────────────────────────┤
│  Profiling Aggregator                        │
│  - 跨进程汇总                                │
│  - 时间线对齐                                │
│  - 热力图生成                                │
├──────────────────────────────────────────────┤
│  Profiling Collector (per process)           │
│  ├─ C++ Profiler: perf / ITT / custom        │
│  ├─ Script Profiler: cProfile / Lua profiler │
│  └─ System Profiler: CPU / MEM / NET / IO    │
├──────────────────────────────────────────────┤
│  Probe Points (编译期可消解)                   │
│  - TICK_START / TICK_END                     │
│  - ENTITY_CREATE / ENTITY_DESTROY            │
│  - MESSAGE_SEND / MESSAGE_RECV               │
│  - SCRIPT_EXEC_START / SCRIPT_EXEC_END       │
│  - DB_QUERY_START / DB_QUERY_END             │
│  - AOI_UPDATE_START / AOI_UPDATE_END         │
└──────────────────────────────────────────────┘
```

### 2.3 核心接口

```cpp
// profile/IProfiler.h

class IProfiler {
public:
    virtual ~IProfiler() = default;

    // 作用域计时器
    virtual ScopeTimer scope(const char* name) = 0;

    // 计数器
    virtual void counter(const char* name, int64_t value) = 0;

    // 直方图（延迟分布）
    virtual void histogram(const char* name, int64_t value) = 0;

    // 标记点（时间线标注）
    virtual void mark(const char* name) = 0;

    // 导出
    virtual void flush() = 0;
};

// 编译期消解宏
#ifdef THESEED_ENABLE_PROFILING
    #define PROFILE_SCOPE(name) auto _prof_scope = g_profiler->scope(name)
    #define PROFILE_COUNTER(name, val) g_profiler->counter(name, val)
    #define PROFILE_HISTOGRAM(name, val) g_profiler->histogram(name, val)
#else
    #define PROFILE_SCOPE(name) ((void)0)
    #define PROFILE_COUNTER(name, val) ((void)0)
    #define PROFILE_HISTOGRAM(name, val) ((void)0)
#endif
```

### 2.4 自动阈值告警

```cpp
// 每个tick结束自动检查
struct TickProfile {
    double total_ms;           // tick 总耗时
    double network_ms;         // 网络处理
    double script_ms;          // 脚本执行
    double aoi_ms;             // AOI 更新
    double db_ms;              // 数据库操作
    double idle_ms;            // 空闲等待
    int entity_count;          // 实体数
    int message_count;         // 消息数
};

// 超阈值自动上报
void checkTickHealth(const TickProfile& p) {
    if (p.total_ms > g_config.tick_budget_ms * 0.8) {
        g_alerts->warn("tick slow", p);
    }
    if (p.script_ms > p.total_ms * 0.7) {
        g_alerts->warn("script heavy", p);
    }
}
```

### 2.5 与 KBEngine 的对比

| 能力 | KBEngine | theseed |
|------|---------|---------|
| CPU 性能分析 | 手动 `Profiler` 类，有限范围 | 零成本 probe + 火焰图 |
| 内存分析 | 无内建 | Entity 内存布局 + 对象池水位 |
| tick 耗时 | `g_componentID` 手动统计 | 自动 tick profile 报告 |
| 脚本性能 | Python `cProfile` 手动挂 | 自动嵌入脚本 profile |
| 网络 I/O | `NetworkStats` 基础统计 | 延迟直方图 + 吞吐曲线 |
| 集群视角 | 无 | 跨进程汇总 dashboard |

---

## 3. 运维自动化

### 3.1 设计目标

- 千台级集群一键部署
- 滚动更新不丢在线玩家
- 异常自愈：进程挂了自动拉起，实体自动迁移
- 配置中心化：一个 YAML 管全部

### 3.2 架构

```
┌───────────────────────────────────────────────────────┐
│  Control Plane (控制面)                                │
│  ├─ ClusterManager: 集群拓扑、进程管理                  │
│  ├─ ConfigCenter: 配置中心（etcd/consul）               │
│  ├─ DeployManager: 部署编排、滚动更新                    │
│  ├─ HealthMonitor: 健康检查、告警                        │
│  └─ AuditLogger: 操作审计                               │
├───────────────────────────────────────────────────────┤
│  Agent (每台物理机)                                     │
│  ├─ ProcessSupervisor: 进程启停、crash 拉起              │
│  ├─ ResourceCollector: CPU/MEM/NET/IO 上报              │
│  ├─ ConfigSync: 配置热拉取                              │
│  └─ LogCollector: 日志采集 + 结构化                      │
├───────────────────────────────────────────────────────┤
│  Data Plane (数据面，引擎进程本身)                        │
│  └─ HealthReporter: 心跳 + tick 健康度 + 实体水位        │
└───────────────────────────────────────────────────────┘
```

### 3.3 滚动更新流程

```
DeployManager.triggerUpdate(version="2.3.1")
    │
    ├─ Phase 1: Pre-check
    │   ├─ 检查所有进程健康
    │   ├─ 检查配置兼容性（def diff）
    │   └─ 生成更新计划（分组、顺序）
    │
    ├─ Phase 2: Drain (逐组)
    │   ├─ 标记进程为 "draining"
    │   ├─ 停止接受新连接
    │   ├─ 等待实体迁移完成（Base entity 转移到其他 BaseApp）
    │   └─ 等待进行中的消息处理完毕
    │
    ├─ Phase 3: Update (逐组)
    │   ├─ Agent 拉取新二进制 + 脚本
    │   ├─ 重启进程
    │   ├─ 健康检查通过后加入集群
    │   └─ 等待实体水位恢复正常
    │
    ├─ Phase 4: Verify
    │   ├─ 全集群健康检查
    │   ├─ 性能指标对比
    │   └─ 自动回滚判定
    │
    └─ Phase 5: Complete / Rollback
```

### 3.4 核心接口

```cpp
// ops/IControlPlane.h

class IControlPlane {
public:
    virtual ~IControlPlane() = default;

    // 集群管理
    virtual ClusterStatus getClusterStatus() = 0;
    virtual std::vector<ProcessInfo> listProcesses(const std::string& role) = 0;

    // 部署
    virtual DeployTask deploy(const DeployConfig& config) = 0;
    virtual DeployTask rollingUpdate(const std::string& version,
                                     const RollingUpdatePolicy& policy) = 0;

    // 热更新（仅脚本/配置）
    virtual HotUpdateTask hotUpdate(const HotUpdateConfig& config) = 0;

    // 实体迁移
    virtual MigrationTask migrateEntities(ProcessId from, ProcessId to) = 0;

    // 告警
    virtual void setAlertRule(const AlertRule& rule) = 0;
};
```

### 3.5 与 KBEngine 的对比

| 能力 | KBEngine | theseed |
|------|---------|---------|
| 进程管理 | Machine (UDP 广播发现) | Agent + Control Plane |
| 部署 | 手动拷贝 + 重启 | 一键滚动更新 |
| 热更新 | Python reload() 有限支持 | 脚本热更 + 配置热更 + 回滚 |
| 实体迁移 | 无自动迁移 | drain → migrate → restart |
| 配置管理 | XML 散落各进程 | 配置中心 + 版本控制 |
| 千台运维 | 不现实 | 分组 + 并行 + 自动回滚 |

---

## 4. 热更新系统

### 4.1 设计目标

- **脚本热更**：修改 Python/Lua 脚战后，不重启进程、不丢失玩家状态
- **配置热更**：修改 def / types / 配置后，立即生效
- **安全回滚**：热更失败可一键回退到上一版本
- **不能热更的场景**明确报错，不允许静默失败

### 4.2 热更新分类

```
热更新
├── L1: 配置热更（最安全）
│   ├── 游戏数值表
│   ├── 匹配规则
│   └── 运营活动配置
│
├── L2: 脚本热更（中等风险）
│   ├── 修改方法实现
│   ├── 添加新方法
│   └── 修改定时器逻辑
│
├── L3: 定义热更（高风险）
│   ├── 添加新属性（有默认值）
│   ├── 添加新 Entity 类型
│   └── 修改索引/约束
│
└── L4: 结构变更（不可热更，需滚动重启）
    ├── 删除属性
    ├── 修改属性类型
    └── 修改网络协议
```

### 4.3 热更新流程

```cpp
// hotupdate/IHotUpdateManager.h

enum class HotUpdateLevel { L1_Config, L2_Script, L3_Def, L4_NeedRestart };

class IHotUpdateManager {
public:
    virtual ~IHotUpdateManager() = default;

    // 分析变更，返回热更级别
    virtual HotUpdateLevel analyze(const DiffResult& diff) = 0;

    // 执行热更
    virtual HotUpdateResult apply(const HotUpdatePackage& pkg) = 0;

    // 回滚
    virtual HotUpdateResult rollback(const std::string& version) = 0;

    // 验证
    virtual ValidationResult validate(const HotUpdatePackage& pkg) = 0;
};
```

### 4.4 脚本热更的安全机制

```python
# 热更时自动执行的验证流程
class HotUpdateValidator:
    def validate_script_change(self, old_class, new_class):
        # 1. 检查是否有正在执行的实例
        active_instances = find_active_instances(old_class)
        if active_instances:
            # 2. 保存当前状态快照
            snapshots = {id: snapshot(inst) for inst in active_instances}

            # 3. 尝试应用新类
            for inst in active_instances:
                migrate_instance(inst, old_class, new_class)

            # 4. 验证迁移后一致性
            for inst in active_instances:
                assert inst.__dict__.keys() >= new_class.__required_attrs__

        return ValidationResult(ok=True)
```

---

## 5. 跨服支持

### 5.1 设计目标

- 玩家可以在不同 Space（可能在不同 CellApp）之间无缝移动
- 跨服调用应当和同进程调用有统一的编程模型
- 跨服不意味着玩家断线重连，而是透明的实体迁移

### 5.2 跨服场景分类

```
跨服
├── Space 内跨 Cell（已有基础）
│   └── 同一 CellApp 或不同 CellApp 的 Cell 之间
│
├── 跨 BaseApp（同类型服务器）
│   └── 实体从一个 BaseApp 迁移到另一个
│
├── 跨服区（Cross-Realm）
│   ├── 玩家从 Realm A 进入 Realm B
│   ├── 需要数据同步 / 代理创建
│   └── 返回时数据合并
│
└── 跨游戏类型（Cross-Game）
    └── 大厅服 ↔ 战斗服 ↔ 休闲服
```

### 5.3 统一通信模型

```cpp
// crossserver/ICrossServerManager.h

// 不管目标在同一进程还是跨服，编程模型一致
class ICrossServerManager {
public:
    virtual ~ICrossServerManager() = default;

    // 实体迁移
    virtual MigrationHandle migrateEntity(EntityId id,
                                          const ServerEndpoint& target) = 0;

    // 跨服代理（不迁移实体，创建远程代理）
    virtual EntityProxy createProxy(EntityId id,
                                    const ServerEndpoint& target) = 0;

    // 跨服查询
    virtual Future<CrossServerResult> query(const CrossServerQuery& q) = 0;

    // 服务发现
    virtual std::vector<ServerEndpoint> discover(const std::string& service) = 0;
};
```

### 5.4 实体迁移流程

```
迁移请求 (BaseApp_A → ControlPlane)
    │
    ├─ 1. 锁定实体（禁止新消息进入）
    │      实体状态: ACTIVE → LOCKED
    │
    ├─ 2. 序列化实体状态
    │      - 所有持久化属性
    │      - 所有 volatile 属性
    │      - 定时器列表
    │      - 进行中的异步操作标记
    │
    ├─ 3. 传输到目标进程
    │      - 通过 ITransport 可靠传输
    │      - 带版本号，防止乱序
    │
    ├─ 4. 目标进程反序列化并创建实体
    │      - 调用 onMigrateIn() 生命周期
    │      - 恢复定时器
    │
    ├─ 5. 更新路由表
    │      - EntityID → 新进程的映射
    │      - 通知所有缓存了旧映射的进程
    │
    ├─ 6. 解锁实体
    │      实体状态: LOCKED → ACTIVE
    │
    └─ 7. 通知客户端（如果需要切换连接）
           - 同进程/同机房: 不需要
           - 跨机房: 可能需要客户端重连
```

### 5.5 与 KBEngine 的对比

| 能力 | KBEngine | theseed |
|------|---------|---------|
| 跨 Cell | 有（Ghost 机制） | 改进 Ghost + 实体迁移 |
| 跨 BaseApp | 有限（需要 DBMgr 中转） | 直接迁移 + 路由更新 |
| 跨服区 | 无内建支持 | CrossRealm 协议 |
| 编程模型 | EntityCall 不区分远近 | 统一，迁移对脚本透明 |
| 迁移安全 | 无锁定机制 | LOCKED 状态 + 路由原子更新 |

---

## 6. 数据定义层

### 6.1 设计目标

- 一份定义文件同时描述：内存布局、网络协议、持久化 schema
- 支持多种存储后端（MySQL、PostgreSQL、MongoDB、Redis、内存）
- 定义变更时有明确的迁移路径
- 人类可读、工具可解析、版本可 diff

### 6.2 定义语言设计

```yaml
# defs/entities/Player.def.yaml

entity Player:
  description: "玩家实体"
  sides: [base, cell]          # 在哪些侧存在

  properties:
    # 基础属性（base + cell + client + db）
    name:
      type: STRING
      size: 64
      flags: [base, cell, persistent]
      index: true              # 数据库索引

    level:
      type: UINT32
      default: 1
      flags: [base, cell, client, persistent]

    hp:
      type: FLOAT32
      default: 100.0
      flags: [cell, client]
      detail_level: 0          # 高精度同步

    position:
      type: VECTOR3
      flags: [cell, client]
      detail_level: 1          # 低精度（远处玩家）

    gold:
      type: UINT64
      flags: [base, persistent]
      shard_key: true          # 分片键

    # 嵌套结构
    equipment:
      type: FIXED_DICT
      fields:
        weapon: { type: UINT32, default: 0 }
        armor: { type: UINT32, default: 0 }
        accessory: { type: UINT32, default: 0 }
      flags: [base, cell, persistent]

  # 方法定义
  methods:
    onLevelUp:
      side: base
      args: [newLevel: UINT32]
      exposed: false           # 客户端不可直接调用

    attack:
      side: cell
      args: [targetId: ENTITY_ID, skillId: UINT32]
      exposed: true            # 客户端可直接调用

    onDamaged:
      side: cell
      args: [damage: FLOAT32, attacker: ENTITY_ID]
      exposed: false

  # 定时器
  timers:
    - name: saveTimer
      interval: 30s
      method: onSave

  # 存储策略
  storage:
    backend: mysql             # 主存储
    cache: redis               # 缓存层
    sharding:
      strategy: hash           # 分片策略
      key: gold                # 分片键
      count: 16                # 分片数
```

### 6.3 类型系统

```yaml
# defs/types.yaml — 自定义类型

types:
  # 基础类型
  UINT8:   { size: 1, network: "uint8" }
  UINT16:  { size: 2, network: "uint16" }
  UINT32:  { size: 4, network: "uint32" }
  UINT64:  { size: 8, network: "uint64" }
  INT8:    { size: 1, network: "int8" }
  INT16:   { size: 2, network: "int16" }
  INT32:   { size: 4, network: "int32" }
  INT64:   { size: 8, network: "int64" }
  FLOAT32: { size: 4, network: "float32" }
  FLOAT64: { size: 8, network: "float64" }
  STRING:  { size: var, network: "string", max: 65535 }
  BLOB:    { size: var, network: "bytes", max: 1048576 }
  BOOL:    { size: 1, network: "uint8" }

  # 复合类型
  VECTOR2: { fields: [x: FLOAT32, y: FLOAT32] }
  VECTOR3: { fields: [x: FLOAT32, y: FLOAT32, z: FLOAT32] }
  VECTOR4: { fields: [x: FLOAT32, y: FLOAT32, z: FLOAT32, w: FLOAT32] }
  ENTITY_ID: { base: UINT64 }

  # 容器类型
  FIXED_DICT: { kind: dict }
  FIXED_ARRAY: { kind: array, max_elements: 65536 }

  # 用户自定义类型
  ITEM_ID: { base: UINT32 }
  SKILL_ID: { base: UINT32 }
```

### 6.4 存储后端抽象

```cpp
// storage/IStorageBackend.h

class IStorageBackend {
public:
    virtual ~IStorageBackend() = default;

    // CRUD
    virtual Future<EntityData> load(EntityId id, const EntityDef& def) = 0;
    virtual Future<void> save(EntityId id, const EntityData& data,
                              const EntityDef& def) = 0;
    virtual Future<void> remove(EntityId id) = 0;

    // 查询
    virtual Future<std::vector<EntityId>> query(const StorageQuery& q) = 0;

    // 批量
    virtual Future<void> batchSave(const std::vector<std::pair<EntityId, EntityData>>& items) = 0;

    // Schema 管理
    virtual Future<void> createTable(const EntityDef& def) = 0;
    virtual Future<MigrationPlan> planMigration(const EntityDef& old_def,
                                                 const EntityDef& new_def) = 0;
    virtual Future<void> executeMigration(const MigrationPlan& plan) = 0;

    // 后端标识
    virtual std::string backendName() const = 0;
    virtual std::vector<std::string> capabilities() const = 0;
};
```

### 6.5 内建后端实现

```
storage/
├── IStorageBackend.h           # 抽象接口
├── MySQLBackend.cpp            # MySQL / MariaDB
├── PostgreSQLBackend.cpp       # PostgreSQL（JSON 支持）
├── MongoDBBackend.cpp          # MongoDB（文档模型）
├── RedisBackend.cpp            # Redis（缓存 + 热数据）
└── MemoryBackend.cpp           # 纯内存（测试 / 临时实体）
```

### 6.6 自动 Schema 迁移

```yaml
# 当 def 变更时，自动生成迁移计划
# 例如：给 Player 添加新属性 "vip_level"

migration:
  from_version: "1.5"
  to_version: "1.6"
  changes:
    - type: add_property
      entity: Player
      property: vip_level
      type: UINT32
      default: 0
      safe: true                # 有默认值，可在线执行

  # 自动生成的 SQL
  auto_sql:
    mysql: "ALTER TABLE tbl_Player ADD COLUMN vip_level INT UNSIGNED NOT NULL DEFAULT 0"
    postgresql: "ALTER TABLE tbl_Player ADD COLUMN vip_level INTEGER NOT NULL DEFAULT 0"
    mongodb: "db.tbl_Player.updateMany({}, {$set: {vip_level: 0}})"
```

### 6.7 与 KBEngine 的对比

| 能力 | KBEngine | theseed |
|------|---------|---------|
| 定义格式 | XML (.def) | YAML（人类可读） |
| 定义合一 | 脚本+协议+持久化分别定义 | 一份 YAML 三合一 |
| 存储后端 | MySQL + Redis | MySQL/PG/Mongo/Redis/Memory |
| Schema 迁移 | 无自动支持 | 自动 diff + 迁移计划 |
| 分片 | 无内建 | 声明式分片策略 |
| 类型安全 | 运行时检查 | 编译期 + 运行时双重检查 |
| 工具支持 | 无 | def lint / def diff / def migrate |

---

## 7. 总体依赖关系

```
                    ┌─────────────┐
                    │   这些都是   │
                    │   横切关注点  │
                    └──────┬──────┘
                           │
         ┌─────────────────┼─────────────────┐
         │                 │                 │
    ┌────▼─────┐    ┌─────▼──────┐    ┌─────▼──────┐
    │  Debug    │    │  Profile   │    │   Ops      │
    │  体系     │    │  体系      │    │   体系     │
    └────┬─────┘    └─────┬──────┘    └─────┬──────┘
         │                │                 │
         └────────────────┼─────────────────┘
                          │
              注入到核心层的接口
                          │
    ┌──────────┬──────────┼──────────┬──────────┐
    │          │          │          │          │
ITransport  IScript    IEntity   IStorage   ICrossServer
```

核心原则：
1. **这些 DX 层不引入核心层依赖**，而是通过 probe/hook 方式注入
2. **Debug 和 Profile 在 release 构建中可完全编译消除**
3. **Ops 层通过 Control Plane 与引擎进程通信**，不侵入引擎核心
4. **所有接口可独立实现和替换**，不强制绑定特定技术栈

---

## 8. 优先级建议

| 阶段 | 内容 | 理由 |
|------|------|------|
| P0 | 数据定义层 + Storage 抽象 | 所有功能的基础，先定好 def 格式 |
| P1 | Profile 体系（probe 点） | 编译期消解，越早埋越好 |
| P1 | Debug 体系（基础 hook） | 开发阶段就需要 |
| P2 | 热更新 L1/L2 | 上线前必须具备 |
| P2 | 运维自动化 Agent | 上线前必须具备 |
| P3 | 跨服支持 | 根据游戏类型需要 |
| P3 | 运维 Dashboard | 运维工具链完善 |
| P4 | 热更新 L3 | 高级需求 |
| P4 | Debug DAP 集成 | 锦上添花 |
