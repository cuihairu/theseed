# MVP Architecture Baseline — 可落地架构基线

> 这篇文档不是替代现有分篇设计，而是为现阶段实现提供统一约束。
> 目标是先做出一个能稳定跑起来、边界清晰、可持续演进的 theseed MVP。

---

## 0.5 引擎实现对照与取舍

### BigWorld 是怎么实现的

```
BigWorld 的做法是“先把运行时和系统层都做厚”：
  - runtime 主干强
  - HA / 数据 / 登录 / 控制面体系完整
  - 但整体偏重，MVP 复刻成本高
```

### KBEngine 是怎么实现的

```
KBEngine 的做法是“先把运行时主干跑通”：
  - tick / entity / AOI / EntityCall 简洁
  - 运行时落地强
  - 但系统层能力偏薄
```

### 优缺点

```
BigWorld 优点：
  - 系统边界完整
  - 运行时与运维链条成体系

BigWorld 缺点：
  - 重
  - 复杂
  - MVP 复制成本高

KBEngine 优点：
  - 轻
  - 易落地
  - 适合做 runtime core 起点

KBEngine 缺点：
  - 系统级能力薄
  - 运维、HA、工具链要自己补
```

### theseed 的取舍

```
theseed 先取 KBEngine 的可落地性，
再按 BigWorld 的系统面补长期边界。

因此 MVP 不追求全量 BigWorld 复刻，
但文档边界要保留 BigWorld 级系统能力的方向。
```

---

## 1. 文档目的

当前设计文档覆盖范围很大，已经包含：

- Base/Cell、Ghost/Witness、AOI、迁移、容错
- XML/XSD 数据定义、多后端存储、合服
- Gateway、消息总线、Redis、异步任务
- 热更新、客户端 SDK、OTel、脚本调试、自动扩缩

这些方向本身没有问题，但在进入实现阶段前，必须先明确：

1. 哪些能力属于 MVP
2. 哪些能力只是 Phase 2 / Phase 3 预留
3. 各模块之间的硬边界是什么
4. 运行时的一致性规则是什么

本文档优先解决这 4 个问题。

---

## 2. MVP 目标

theseed 的 MVP 不追求“完整替代 BigWorld”。
MVP 只追求一件事：

**做出一个单机房内可运行的、单 Realm 的、可支撑房间制/副本制/中小地图玩法的分布式游戏服务器内核。**

MVP 明确支持：

- Base/Cell 双体模型
- 单线程 tick 运行时
- SingleCell Space
- 十字链表 AOI
- Witness 视野同步
- Base ↔ Cell EntityCall
- MySQL 持久化
- XML/XSD 数据定义
- Gateway 接入
- Metrics + Logging
- Unity 客户端代码生成

MVP 明确不支持：

- 动态多 Cell 拓扑
- 跨机房跨 Realm 迁移
- 脚本栈迁移
- L3/L4 热更新
- 多数据库后端同时齐备
- UE5/Cocos 全量工程级支持
- 自动扩缩容闭环

---

## 3. 第一原则

### 3.1 Runtime First

优先保证运行时正确性，再谈平台能力。

排序如下：

1. Tick 一致性
2. Entity 生命周期正确
3. 消息路由正确
4. 属性同步正确
5. 持久化正确
6. 可观测性可用
7. 运维能力增强

### 3.2 单一事实来源

同一种职责只能有一个主通道：

- 实体运行时消息，只能走 Runtime Transport
- 控制面消息，只能走 Control Plane
- 跨 Realm 异步消息，只能走 Cross-Realm Bridge

禁止同一类消息同时走两条链路。

### 3.3 实现优先于愿景

凡是会显著增加状态一致性难度的能力，默认不进入 MVP：

- 协程栈迁移
- 任意脚本热替换
- 在线协议变更
- 动态拓扑自动 rebalance
- 跨后端统一高级查询语义

---

## 4. 三个平面

当前文档最需要统一的是通信边界。MVP 固定为三个平面。

### 4.1 Runtime Data Plane

职责：

- EntityCall
- Base ↔ Cell 控制消息
- 实体创建/销毁
- Real → Ghost 同步
- Witness → Client 同步前的服务端内部转发
- 迁移过程中的运行时数据传输

要求：

- 保序
- 低延迟
- 有背压
- 可感知实体路由

结论：

**Runtime Data Plane 不走 MessageBus。**

### 4.2 Control Plane

职责：

- 进程上下线
- 路由表发布
- 配置变更广播
- 部署、drain、重启命令
- 监控与告警事件

特点：

- 不是 tick 内关键路径
- 允许额外跳数
- 更关注可观测和管理

### 4.3 Cross-Realm Async Plane

职责：

- 跨服匹配
- 跨服查询
- 跨服邮件/通知
- 跨机房桥接

特点：

- 不是实体本地权威路径
- 允许显式超时、重试、补偿
- 与 Runtime 生命周期解耦

---

## 5. 并发与线程模型

MVP 采用严格的线程约束。

### 5.1 Entity 所有权

每个 Entity 在任意时刻只属于一个 tick 线程。

规则：

- 只有 owning tick 线程可以读写 Entity 内存
- PropertyBlock 不是并发容器
- Witness、Ghost、Controller、Timer 都跟随 Entity 所在线程

### 5.2 异步回调规则

异步任务可以在后台线程执行，但结果只能以“投递事件”的方式回到 tick 线程。

禁止：

- 在后台线程直接修改 Entity
- 在 tick 线程阻塞等待 `Future.get()`
- 在脚本回调里同步等待外部 I/O

允许：

- DB 线程完成后投递 `onDbLoaded`
- HTTP 线程完成后投递 `onHttpResponse`
- 跨服请求完成后投递 `onCrossRealmResult`

---

## 6. Tick 与同步基线

MVP 统一采用下面的时序模型：

1. Network Ingress
2. Timer
3. Entity Logic
4. Script Callback
5. Sync Build
6. Flush

关键约束：

- tick 内允许改属性
- tick 内只记脏，不立即外发
- tick 末统一构造同步包
- tick 末统一 flush

这条规则覆盖：

- Real → Ghost
- Real → Witness
- Witness → Client

例外只有两类：

- 明确的 RPC/事件消息
- 生命周期控制消息（创建、销毁、迁移控制）

属性复制不是 RPC，不应在脚本执行中即时发送。

---

## 7. 可靠性语义

MVP 不实现文档里“逻辑四级、底层四级”的完整模型。
MVP 只承诺两类语义：

### 7.1 Ordered Reliable

用于：

- EntityCall
- 实体创建/销毁
- Base ↔ Cell 控制消息
- 迁移数据
- 路由更新

语义：

- 保序
- 可靠送达
- 有背压

### 7.2 Unordered Lossy

用于：

- 位置/朝向等 volatile 更新
- 可重算的 AOI 辅助通知

语义：

- 可丢弃
- 不重传
- 被下一帧覆盖

如果后续要扩展 `LOW/HIGH/CRITICAL`，应在 Runtime 先实现明确队列、确认与重放机制，再升级文档抽象。

---

## 8. 迁移基线

MVP 只支持两类迁移：

- 单 Realm 内 Cell 迁移
- 跨 Space 传送

MVP 不支持：

- 跨机房迁移
- 挂起脚本协程迁移
- 外部 I/O 上下文迁移

迁移时可序列化状态只包括：

- PropertyBlock
- Space / Position / Direction
- Controller 状态
- Timer 状态
- Witness 基础状态

不进入迁移快照的内容：

- Python/Lua 调用栈
- `yield` 中的 Future continuation
- 正在执行的 HTTP/DB 请求上下文

对应策略：

- 迁移前冻结实体收件箱
- 将未完成异步任务标记为失效或等待回调后重试
- 回调到达时按 Entity epoch 校验是否仍然有效

---

## 9. 热更新基线

MVP 只支持：

- L1 配置热更
- 受限的 L2 脚本实现替换

MVP 不支持：

- L3 def 热更
- L4 结构热更

L2 的约束必须非常严格：

- 只能替换方法实现
- 不修改方法签名
- 不修改 exposed 协议
- 不改变属性布局
- 不依赖栈上旧 frame 的语义兼容

如果脚本更新影响实体协议、属性定义、序列化布局，一律升级为滚动发布，不走热更。

---

## 10. 存储边界

MVP 先不要追求“一个接口覆盖所有后端能力”。

建议拆成最小接口集合：

```cpp
class IEntityStore {
public:
    virtual Future<EntityData> load(EntityId id, const EntityDef& def) = 0;
    virtual Future<void> save(EntityId id, const EntityData& data, const EntityDef& def) = 0;
    virtual Future<void> remove(EntityId id) = 0;
};

class IEntityQueryStore {
public:
    virtual Future<std::vector<EntityId>> query(const StorageQuery& q) = 0;
};

class ISchemaMigrator {
public:
    virtual Future<MigrationPlan> planMigration(const EntityDef& oldDef,
                                                const EntityDef& newDef) = 0;
    virtual Future<void> executeMigration(const MigrationPlan& plan) = 0;
};
```

MVP 默认后端：

- 主存储：MySQL
- 缓存：Redis

MVP 默认映射策略：

- 标量类型：原生列
- VECTOR3：展开列
- FIXED_DICT：JSON
- ARRAY：JSON

`EXPAND` / `SUBTABLE` 保留为能力设计，但不要求首版完整覆盖所有组合。

---

## 11. 客户端与工具链范围

MVP 客户端只保证 Unity 端全链路可用：

- 登录
- 断线重连
- 实体创建/销毁
- 属性同步
- 位置插值
- exposed 方法调用

UE5/Cocos 在文档中可以保留为目标，但实现优先级晚于 Unity。

代码生成首版只承诺：

- Entity 定义生成
- exposed 方法桩生成
- 基础序列化代码生成

不承诺首版就完成：

- Blueprint 深度集成
- 三端编辑器与脚本调试工具等价支持
- 复杂预测回滚框架

---

## 12. 可观测性基线

MVP 可观测性只要求三件事：

1. 结构化日志
2. 基础 Metrics
3. 关键链路 Trace

必做指标：

- tick duration
- entity count
- queue backlog
- transport backpressure
- db load/save latency
- script error count

MVP 可选项：

- 全量脚本断点调试
- 自动扩缩容
- 火焰图导出

可观测性必须服务于 Runtime 调试，而不是先追求平台完整度。

---

## 13. MVP 实施顺序

### Phase A：跑通最小闭环

- TickScheduler
- Entity / BaseCell 模型
- PropertyBlock
- Base ↔ Cell EntityCall
- SingleCell Space
- Witness 同步
- MySQL load/save
- Gateway 登录接入

### Phase B：补齐在线玩法基础

- AOI 十字链表
- Controller
- Redis 会话/限流
- Unity codegen
- 基础 Metrics / Logs

### Phase C：增强运维能力

- 受限脚本热更
- 迁移路由窗口
- 备份恢复原型
- 基础 Trace

只有 Phase A 稳定后，才应该推进：

- 静态多 Cell
- 跨服桥接
- 自动扩缩
- 多存储后端

---

## 14. 与现有文档的关系

本文档对现有设计的约束如下：

- `2-replication-and-space/03-runtime-communication-and-transport` 中 Runtime Data Plane 的边界优先于 MessageBus 扩展表述
- `2-replication-and-space/04-property-replication` 以 tick 末统一 flush 为准
- `2-replication-and-space/05-entity-migration` 不承诺脚本栈迁移
- `4-data-and-ops/02-persistence` 的多能力接口应按实现阶段拆分
- `5-access-and-control-plane/02-message-bus-and-cross-realm` 不再承载 EntityCall 主路径
- `7-scripting-and-client/02-hot-update` 的 MVP 仅支持 L1 + 受限 L2
- `7-scripting-and-client/03-script-debug` 的 MVP 只要求 traceback / stack dump / Entity 上下文 inspect
- `7-scripting-and-client/04-client-sdk` 的 MVP 以 Unity 为主
- `5-access-and-control-plane/05-telemetry-and-debug` 的 MVP 只要求 logs + metrics + key traces

如果后续分篇文档与本页冲突，以本页为当前实现基线，直到对应分篇被修订。
