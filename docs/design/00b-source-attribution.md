# Source Attribution — 设计文档的来源追溯

> 这份文档回答：theseed 的每个设计决策，有多少来自 BigWorld、多少来自 KBEngine、多少是自己加的。
> 诚实标注，不留面子。

---

## 00-runtime-core.md — Runtime Core

| 模块 | 来源 | 说明 |
|------|------|------|
| **Tick Model（单线程 5 阶段）** | KBEngine `EventDispatcher::processUntilBreak` | 5 阶段分段是从 KBEngine 主循环拆出来的，BigWorld 类似但没这么清晰分段 |
| **Entity Base/Cell 双身体** | BigWorld 原创设计 | KBEngine 完整继承。Ch23 对照表明确："核心一致：Base/Cell 分离、real/ghost 区分、Witness 机制——这些都是 BigWorld 架构的核心贡献" |
| **PropertyBlock 连续内存** | **自己加的** | KBEngine 用 Python dict 存属性。PropertyBlock 是 theseed 的改进，灵感来自 redesign.md 中提到的 PropertyBlock 建议 |
| **DirtyMask 位图** | KBEngine `setDirty()` 机制 | KBEngine 已有脏标记概念，theseed 用位图替代了分散的 bool 标记 |
| **AOI 十字链表** | BigWorld 原创设计 | KBEngine 完整继承。Ch23："两套项目都使用十字链表作为 AOI 底层数据结构" |
| **RangeTrigger 边界检测** | BigWorld 原创设计 | KBEngine 完整继承。`RangeTrigger.h/cpp` 两边结构几乎一致 |
| **Witness 视野系统** | BigWorld 原创设计 | KBEngine 继承但简化。BigWorld 有优先级队列+带宽预算，KBEngine 去掉了 |
| **detailLevel 分级同步** | BigWorld DataLoDLevels | KBEngine 继承为 3 级。BigWorld 有 4 级 + hysteresis 防抖 |
| **Hysteresis 防抖** | BigWorld | KBEngine 继承（`viewHysteresisArea`），是视野边界抖动的经典解决方案 |
| **Volatile 属性** | BigWorld VolatileInfo | KBEngine 继承。位置/朝向变化超阈值才同步 |
| **aliasID 压缩** | BigWorld IDAlias | KBEngine 继承。1 字节代替 2 字节传输属性/实体 ID |
| **Ghost real/ghost 模型** | BigWorld 原创设计 | KBEngine 继承。Ch23："Cell 的轻量副本——相同" |
| **GhostManager 路由窗口** | KBEngine 特有 | KBEngine 源码注释："如果期间有base的消息发送过来，entity的ghost机制能够转到real上去"。BigWorld 的 BSP 拓扑用不同方式处理 |
| **isReal() 权威判断** | BigWorld + KBEngine | 两边都有。KBEngine 用 `realCell_==0`，BigWorld 用 `pReal_!=NULL` |
| **EntityCall** | BigWorld Mailbox | "名异实同"（Ch23）。KBEngine 砍掉了 TwoWay，只保留单向 |
| **EntityCall 走 Runtime Transport** | **自己的判断** | BigWorld 用 UDP+Mercury 直连，KBEngine 用 TCP 直连。这些ed 加了"不走 MessageBus"的明确决策 |
| **迁移 5 阶段流程** | KBEngine 为基础 | KBEngine 的 teleport + Ghost 路由窗口。BigWorld 的 Offload 机制类似但更复杂（BSP 拓扑下） |
| **TimerWheel** | 通用算法 | 不是从 BigWorld/KBEngine 来的。KBEngine 用 `ScriptTimers`（时间堆），theseed 选时间轮 |
| **对象池** | KBEngine `OBJECTPOOL_POINT` | KBEngine 已有 MemoryStream/Witness 对象池。theseed 继续使用 |

**小结：Runtime Core 约 70% 来自 BigWorld/KBEngine，20% 是改进（PropertyBlock、DirtyMask 位图），10% 是自己的判断（EntityCall 不走 MQ、TimerWheel 选型）**

---

## 01-developer-experience.md — 开发体验

| 模块 | 来源 | 说明 |
|------|------|------|
| **Debug DAP 协议** | **自己加的** | KBEngine 只有 Telnet `pyExec`。BigWorld 也只有基础调试 |
| **Profile 零成本 probe** | **自己加的** | KBEngine 有 `Profiler` 类但不可编译消除。BigWorld 有三级 Profiler（EntityType/Entity/Cell）但只用于负载均衡 |
| **滚动更新 5 阶段** | **自己加的** | KBEngine 无内建。BigWorld 有 Reviver 但只做进程拉起，不做滚动更新 |
| **热更新 L1-L4 分级** | KBEngine `Python reload()` + **自己扩展** | KBEngine 有脚本热重载但无分级模型。L1-L4 是 theseed 自己的抽象 |
| **跨服迁移** | **自己加的** | KBEngine 无跨服。BigWorld 有 Cell 间 Offload 但不是跨服概念 |
| **数据定义 YAML 三合一** | **自己加的** | KBEngine 用 XML `.def` 分别定义脚本/协议/存储。BigWorld 类似。YAML 三合一是 theseed 的改进 |
| **多存储后端** | KBEngine MySQL+Redis + **扩展** | KBEngine 已有 MySQL+Redis。BigWorld 有 MySQL+XML+Primary/Secondary。theseed 加了 PostgreSQL/MongoDB/Memory |
| **Schema 自动迁移** | **自己加的** | KBEngine 无自动迁移。BigWorld 有 `consolidate_dbs / transfer_db / sync_db` 三个工具 |

**小结：这篇约 20% 来自 KBEngine（热重载、MySQL+Redis），80% 是自己加的。BigWorld 在持久化工具上有参考价值但模型不同**

---

## 02-observability.md — 可观测性

| 模块 | 来源 | 说明 |
|------|------|------|
| **OTel 三支柱** | **自己加的** | KBEngine 有 `Watcher`（有限）和 `ProfileVal`。BigWorld 有 `ForwardingWatcher`（分布式查询）+ 三级 Profiler + 结构化日志。但都没有 OTel |
| **EntityCall Trace Context 传播** | **自己加的** | 把 OTel 的 Context Propagation 映射到 EntityCall。这是这些ed 最原创的洞察之一 |
| **尾部采样** | OTel 标准 | 不是来自游戏引擎 |
| **Metrics 指标清单** | KBEngine `Watcher` + **扩展** | KBEngine 有基本的 tick/entity 指标。theseed 扩展了完整的指标体系 |
| **Grafana Dashboard** | **自己加的** | KBEngine/BigWorld 都没有内建 Dashboard |
| **Alertmanager 告警** | **自己加的** | KBEngine/BigWorld 都没有结构化告警 |
| **Prometheus 集成** | **自己加的** | 通过 OTel Prometheus exporter 标准集成 |
| **tick 级粒度** | KBEngine tick 模型 | tick 作为可观测性单位来自 KBEngine 的设计 |

**小结：这篇约 5% 来自 KBEngine（指标基础概念），95% 是自己加的云原生可观测性。BigWorld 的 ForwardingWatcher 有参考价值但技术栈完全不同**

---

## 03-infrastructure.md — 基础设施

| 模块 | 来源 | 说明 |
|------|------|------|
| **Gateway（TLS/限流/WS）** | **自己加的** | KBEngine 的 LoginApp 只做 TCP 代理。BigWorld 的 LoginApp 类似。这些ed 加了完整的网关层 |
| **MessageBus (NATS)** | **自己加的** | KBEngine 内部直连 TCP。BigWorld 内部 UDP+Mercury 直连。没有用 MQ 的先例 |
| **Redis 封装** | KBEngine 已有 Redis + **扩展** | KBEngine 有基础 Redis 支持。theseed 扩展了分布式锁/排行榜/限流 |
| **异步 Future/Promise** | **自己加的** | KBEngine 用 CallbackMgr。BigWorld 用 Twisted Deferred。theseed 选 Future/Promise |
| **跨服 Realm Bridge** | **自己加的** | 两边都没有跨服概念 |
| **日志收集** | **自己加的** | KBEngine 用文件日志。BigWorld 有 message_logger 进程做聚合 |

**小结：这篇约 10% 来自 KBEngine（Redis 基础），90% 是自己加的。BigWorld 的 message_logger 有一点参考**

---

## 04-client-sdk.md — 客户端 SDK

| 模块 | 来源 | 说明 |
|------|------|------|
| **一份 def → 多端生成** | **自己加的** | KBEngine 没有代码生成器，客户端代码手写。BigWorld 也手写 |
| **属性插值** | **自己加的** | KBEngine 客户端不做插值。BigWorld 有一些但不是内建的 |
| **detailLevel / aliasID** | KBEngine/BigWorld 同步协议 | 这些是服务端已有的同步优化，SDK 需要理解它们 |
| **Exposed 方法** | KBEngine `exposed=true` | KBEngine 已有这个安全边界 |
| **断线重连** | KBEngine `rndUUID` + **扩展** | KBEngine 有基础重连（rndUUID + EntityLog）。theseed 加了指数退避+自动重连 |
| **编辑器集成** | **自己加的** | KBEngine/BigWorld 都没有编辑器工具 |
| **UE5 Blueprint 支持** | **自己加的** | KBEngine 无 UE5 支持 |
| **Cocos TypeScript** | **自己加的** | KBEngine 无 Cocos 支持 |

**小结：这篇约 15% 来自 KBEngine（exposed、同步协议），85% 是自己加的**

---

## 05-script-security.md — 脚本安全

| 模块 | 来源 | 说明 |
|------|------|------|
| **L1-L4 四层安全** | **自己加的** | KBEngine/BigWorld 都没有分层安全模型 |
| **Def 校验器** | **自己加的** | KBEngine 在运行时才做类型检查。theseed 提前到编译期 |
| **沙箱白名单** | **自己加的** | KBEngine 的 Python 没有任何沙箱 |
| **超时保护** | **自己加的** | KBEngine 无脚本超时保护 |
| **频率限制** | **自己加的** | KBEngine 无 Exposed 方法频率限制 |
| **热更验证** | **自己加的** | KBEngine 热更无验证流程 |

**小结：这篇约 0% 来自 KBEngine/BigWorld，100% 是自己加的**

---

## 总览

```
来源占比：

                         BigWorld   KBEngine   自己加的
                         ────────   ─────────  ────────
00-runtime-core           35%        35%        30%
01-developer-experience    5%        15%        80%
02-observability           3%         2%        95%
03-infrastructure          0%        10%        90%
04-client-sdk              5%        10%        85%
05-script-security         0%         0%       100%
```

## 结论

**真正来自 BigWorld 的核心贡献**：
- Base/Cell 双身体模型
- 十字链表 AOI
- Ghost real/ghost 权威模型
- Witness + RangeTrigger + detailLevel + Hysteresis
- Mailbox（EntityCall 前身）
- 优先级队列 + 带宽预算（KBEngine 砍掉了这部分）

**真正来自 KBEngine 的贡献**：
- tick 5 阶段分段
- EntityCall 单向简化
- GhostManager 路由窗口（迁移消息保序）
- 对象池模式
- MySQL + Redis 双后端
- exposed 方法信任边界

**这些ed 自己加的（最多）**：
- OTel 可观测性全栈
- Gateway / MessageBus / 跨服
- 客户端代码生成器
- PropertyBlock 连续内存
- 脚本安全四层模型
- 运维自动化（滚动更新/热更分级）

**问题**：自己加的部分太多，BigWorld/BigWorld 的部分集中在 00-runtime-core.md。其他 5 篇几乎全是"云原生扩展"。

**应该补的 BigWorld 参考**：
1. BigWorld 的 `BackupSender` 灾备机制（01 文档缺失）
2. BigWorld 的 `aoi_update_schemes` 可插拔策略（00 文档缺失）
3. BigWorld 的 BSP 树动态拓扑（00 文档只提了简化版）
4. BigWorld 的三级 Profiler 数据驱动负载均衡（02 文档缺失）
5. BigWorld 的 Mercury UDP 四级可靠性（EntityCall 传输层选型未参考）
6. BigWorld 的 `consolidate_dbs / transfer_db / sync_db` 迁移工具（01 文档可参考）
7. BigWorld 的 `LoginApp Challenge` 认证机制（03 Gateway 可参考）
