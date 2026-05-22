# Source Attribution — 来源追溯与覆盖判断

> 这份文档回答三件事：
>
> 1. 新分层下每一层主要继承自 BigWorld、KBEngine，还是 theseed 自己扩展  
> 2. 当前文档覆盖的是“KBEngine 运行时主干”还是“BigWorld 服务端系统面”  
> 3. 哪些能力现在只是边界，不是 MVP 承诺

---

## 0.5 引擎实现对照与取舍

### BigWorld 是怎么实现的

```
BigWorld 的参考价值不只在 runtime，
更在系统层和工具链：
  - HA
  - 数据运维
  - 登录运维
  - Watcher / profiler / controlled shutdown
```

### KBEngine 是怎么实现的

```
KBEngine 的参考价值主要在 runtime 主干：
  - tick
  - entity
  - AOI / ghost / witness
  - migration
```

### 优缺点

```
BigWorld 的优点：
  - 系统面厚
  - 能帮助 theseed 立长期边界

KBEngine 的优点：
  - runtime 落地更直接
  - 更适合作为 MVP 起点

共同缺点：
  - 如果不做来源追溯，文档容易误判“已经覆盖了什么”
```

### theseed 的取舍

```
theseed 这份来源追溯文档的任务，
就是持续提醒读者：
  - 哪些来自 BigWorld
  - 哪些来自 KBEngine
  - 哪些是 theseed 新增
  - 哪些只是边界，不是当前承诺
```

## 2.1 新增基础设施边界

### 仓库布局与构建

```
monorepo / vcpkg / C++23 这组约束主要是 theseed 自己的设计选择，
BigWorld 和 KBEngine 只提供组织形态上的参考，不提供现成的现代仓库模型。
```

### MachineAgent / Host Ops

```
这部分的参考来源是 BigWorld bwmachined + KBEngine machine。
theseed 的收敛方式是：
  - 保留节点代理
  - 保留本机进程编排
  - 保留节点摘要上报
  - 但把全局控制面和诊断面分离出去
```

---

## 1. 总判断

一句话总结：

```
theseed 现在的文档结构，
已经不再只是“KBEngine runtime core + 现代化扩展”，
而是开始按 BigWorld 14.4.1 的服务端系统面来组织边界。
```

但要区分两层：

```
MVP 可落地起点
  - 更接近 KBEngine 风格运行时主干

长期系统边界
  - 明确对标 BigWorld 的 HA、数据运维、登录运维、控制面、load feedback
```

---

## 2. 分层来源

### 2.1 0-foundation

主要来源：

```
theseed 自己定义
```

说明：

```
审计口径、分层原则、MVP 基线
都不是 BigWorld / KBEngine 直接提供的现成文档结构，
而是这轮重组为避免边界混乱而补出的治理层。
```

### 2.2 1-runtime-model

| 主题 | 主要来源 | 说明 |
|------|------|------|
| Tick 5 阶段分段 | KBEngine | `EventDispatcher::processUntilBreak` 风格最清晰 |
| Base / Cell 双体 | BigWorld | KBEngine 是完整继承者 |
| IORuntime / Reactor-Completion 分层 | theseed | 老引擎只有事件循环/ poller 语义，没有这层总抽象 |
| Merkle diff / semantic compatibility | theseed | 老引擎多为单 digest 对比，缺少分层差异定位 |
| PropertyBlock 连续内存 | theseed | 用于替代 Python dict 属性布局 |
| DirtyMask 位图 | KBEngine 思想 + theseed 实现 | 脏标记概念来自现有引擎，位图化是 theseed 收敛 |
| TimerWheel | 通用算法 + theseed 选型 | 非 BigWorld / KBEngine 原生 |
| 对象池模式 | KBEngine | `OBJECTPOOL_POINT` 一类模式延续 |

判断：

```
这一层是“BigWorld 实体模型 + KBEngine 主循环拆解 + theseed 内存改造”。
```

### 2.3 2-replication-and-space

| 主题 | 主要来源 | 说明 |
|------|------|------|
| AOI 十字链表 / RangeTrigger | BigWorld | KBEngine 完整继承 |
| Witness / Ghost / real-ghost 权威 | BigWorld | KBEngine 继承但简化 |
| EntityCall / Mailbox 思想 | BigWorld | KBEngine 做了单向化简 |
| 迁移窗口与路由期 | KBEngine | GhostManager 路由窗口表达更直接 |
| Runtime Data Plane 不走 MQ | theseed | 基于两套引擎直连模式作出的明确判断 |
| AoI update scheme / load bounds | BigWorld | KBEngine 基本没有等价系统面 |
| BSP rebalance / offload | BigWorld | 是和 KBEngine 拉开差距的关键层 |

判断：

```
这一层是当前最“BigWorld 对齐”的核心层。
```

### 2.4 3-cluster-and-availability

| 主题 | 主要来源 | 说明 |
|------|------|------|
| Reviver / supervisor 思想 | BigWorld | KBEngine 较弱 |
| BackupHash / BackupHashChain | BigWorld | KBEngine 基本缺失 |
| Service Fragment / Retire / Controlled Shutdown | BigWorld | KBEngine 没有成体系实现 |
| EntityProfiler → load feedback | BigWorld | 是 BigWorld 的系统级差异点 |
| 当前 MVP 只保留边界 | theseed | 不把远期 HA 伪装成当前能力 |

判断：

```
这一层几乎完全是按 BigWorld 服务端系统层来立边界。
```

### 2.5 4-data-and-ops

| 主题 | 主要来源 | 说明 |
|------|------|------|
| 属性级存储映射思想 | BigWorld | 比 KBEngine 的 BLOB 化更完整 |
| MySQL 主路径的现实起点 | KBEngine | 更适合 MVP 可落地 |
| LocalArchiveStore / SecondaryDB 语义 | BigWorld | 重点是职责，不是具体后端 |
| snapshot / transfer / consolidate / sync / repair | BigWorld 工具链 | `transfer_db / consolidate_dbs / sync_db` |
| JSON、多后端、Schema 自动迁移 | theseed | 现代化扩展，不是照抄旧引擎 |

判断：

```
这一层是“BigWorld 工具链边界 + theseed 现代存储增强”。
```

### 2.6 5-access-and-control-plane

| 主题 | 主要来源 | 说明 |
|------|------|------|
| Login Challenge / statusCheck / ban 清理 | BigWorld | 登录面最重要的系统参考 |
| LoginApp / Gateway 接入基线 | BigWorld + KBEngine | 旧引擎都只给了较薄的接入面 |
| MessageBus / Cross-Realm | theseed | 两套引擎都没有真正的现代 MQ 控制面 |
| Redis / Async / Config | KBEngine 基础 + theseed 扩展 | KBEngine 只提供基础支撑 |
| Watcher 式控制面抽象 | BigWorld | 但实现栈改为这些ed 自己设计 |
| OTel / Telemetry / Diagnostics | theseed | 现代化观测面扩展，脚本执行态调试单独拆出 |

判断：

```
这一层不是单纯“云原生外挂”，
而是把 BigWorld 登录运维面与 Watcher 控制面重新放回系统边界。
```

### 2.7 6-world-and-game-framework

| 主题 | 主要来源 | 说明 |
|------|------|------|
| Physics / Navigation / Controllers | 通用设计 + theseed | 两套引擎可参考但并非同一抽象 |
| World Streaming / Compiled Space | BigWorld | 这是 BigWorld 世界系统的重要补齐 |
| Built-in Entities | theseed | 更偏游戏框架层 |
| Lifecycle / Script Binding | BigWorld / KBEngine 思想 + theseed 收敛 | 钩子表与自动绑定是整理结果 |

判断：

```
这一层主要是“世界系统边界归位”，
不再把它们笼统叫做 gameplay。
```

### 2.8 7-scripting-and-client

| 主题 | 主要来源 | 说明 |
|------|------|------|
| 热更新基础思路 | KBEngine reload + theseed 扩展 | L1-L4 分级是 theseed 自己定义 |
| Exposed 安全边界 | KBEngine | 客户端调用授权边界延续 |
| 脚本执行态调试 | theseed | BigWorld / KBEngine 主要是 traceback、watcher、reload，不是成体系断点调试 |
| 多端代码生成 / Unity 优先 | theseed | 两套老引擎都没有现代 codegen 结构 |
| 脚本安全层级 | theseed | 几乎完全新增 |

判断：

```
这一层继承较少，更多是 theseed 自己的产品化层。
```

---

## 3. 覆盖结论

### 3.1 以 KBEngine 为标尺

当前文档已经覆盖：

```
  - 单线程 tick 主循环
  - Base / Cell
  - AOI / Witness / Ghost
  - EntityCall
  - 属性同步
  - 单 Realm 持久化主路径
  - Unity 优先的客户端主链路
```

所以如果目标是：

```
做出一个比 KBEngine 更清晰、边界更现代的运行时核心
```

当前文档已经够用了。

### 3.2 以 BigWorld 为标尺

当前文档已经明确覆盖设计边界：

```
  - BackupHash / 热备恢复链
  - SecondaryDB / LocalArchiveStore
  - Data Ops Toolchain
  - Login 运维面
  - Watcher 式控制面
  - Runtime profiler → 调度反馈
  - BSP / Load Bounds / Offload
  - World Streaming / Compiled Space
```

但要强调：

```
这些很多仍然是 Phase 2 / Phase 3 边界，
不是 MVP 实现承诺。
```

---

## 4. 当前能力判断

| 能力域 | 覆盖状态 | 备注 |
|------|------|------|
| Base / Cell / Tick / AOI / Ghost / Witness | 已覆盖 | 可作为 MVP 运行时主干 |
| EntityCall / Property Replication / Migration | 已覆盖 | 口径已统一到复制与空间层 |
| SingleCell / Static Partition 边界 | 已覆盖 | Phase 1 / 2 清晰 |
| BigWorld 级 BSP / grow-shrink / auto rebalance | 已定义边界 | 非 MVP |
| 主持久化 / 数据定义 / 查询 | 已覆盖 | MVP 可落地 |
| LocalArchiveStore / SecondaryDB | 已定义边界 | 非 MVP |
| BackupHash / 热备恢复链 | 已定义边界 | 非 MVP |
| Login Challenge / 登录运维面 | 已覆盖边界 | BigWorld 对齐点 |
| Watcher / 控制面 / 在线命令 | 已覆盖边界 | 不再混在 OTel 里 |
| Runtime profiler → 调度反馈闭环 | 已覆盖边界 | BigWorld 关键差异 |

---

## 5. 总结

真正来自 BigWorld 的最大贡献，现在主要体现在三层：

```
2-replication-and-space
3-cluster-and-availability
4-data-and-ops
```

真正来自 KBEngine 的现实落地价值，主要体现在两层：

```
1-runtime-model
以及 2-replication-and-space 的 MVP 起步形态
```

theseed 自己新增最多的层，则是：

```
5-access-and-control-plane
7-scripting-and-client
```

但这轮重组后的关键变化是：

```
这些新增能力不再漂浮在旧目录里，
而是被放回了和 BigWorld / KBEngine 对照后更合理的系统层级。
```
