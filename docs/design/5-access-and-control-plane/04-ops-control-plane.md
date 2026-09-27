# Ops Control Plane — 运维控制面与分布式观察

> 来源源头：BigWorld `Watcher / ForwardingWatcher / EntityProfiler / challenge watcher / controlled shutdown`。
> 参考实现：KBEngine `Watcher / WebConsole` 的轻量管理路径。
> 这层不是 OTel，也不是 Runtime Data Plane，而是在线运维与控制的独立平面。

---

## 0.5 引擎实现对照与取舍

### BigWorld 是怎么实现的

```
BigWorld 的 Watcher 体系不是简单监控：
  - 分布式查询
  - 在线命令
  - 配置热改
  - 状态检查
  - 受控停服入口
```

### KBEngine 是怎么实现的

```
KBEngine 也有 Watcher / WebConsole 一类能力，
但运维控制面没有 BigWorld 那么系统化。
```

### 优缺点

```
BigWorld 的优点：
  - 运维控制面完整
  - 远程诊断能力强

KBEngine 的优点：
  - 简单
  - 实现成本低

共同缺点：
  - 一旦把控制面和观测面混写，边界会迅速失控
```

### theseed 的取舍

```
theseed 把 Ops Control Plane 独立出来，
明确它负责 Inspect + Command + Config + Audit，
不让它退化成“又一个监控面板”。
```

### 为什么不能把探活和状态公开都塞进 Telemetry

```
Telemetry 的核心是采集与诊断输出。
Ops 的核心是：
  - 当前能不能接流量
  - 当前能不能执行命令
  - 当前状态能不能作为运维决策输入

所以 /health 和 /status/summary 虽然“看起来像观测接口”，
但本质上属于控制面公开边界。
```

---

## 0. 设计边界

```
Ops Control Plane 负责：
  - 远程查询运行时状态
  - 远程执行受控命令
  - 在线调整少量安全配置
  - 对运维动作进行审计

Ops Control Plane 不负责：
  - EntityCall 主路径
  - ghost / witness / 迁移数据传输
  - metrics / traces / logs 存储后端
  - 游戏业务跨服消息
```

换句话说：

```
OTel 负责 Telemetry
Ops Control Plane 负责 Inspect + Command + Config + Audit
```

---

## 1. 为什么不能只写 OTel

BigWorld 的 `Watcher` 不是单纯指标系统。

它同时承担了：

```
1. 分布式状态树查询
   - 进程、实体、Cell、Space、DB、登录挑战配置

2. 在线命令入口
   - statusCheck
   - clear IP bans
   - retire / drain / controlled shutdown

3. 在线配置热改
   - challengeType
   - 采样、阈值、限流

4. 负载诊断
   - entity profiler
   - entity type profiler
```

如果把这些都塞进 OTel 文档，会把“观测”和“控制”混成一层。

更严格的规则可以写成：

```
给人看火焰图、trace、metric
  → Telemetry

给平台看 ready / live / startup
  → Ops Control Plane

给操作员下 set draining / retire / shutdown
  → Ops Control Plane
```

---

## 2. 能力范围

### 2.1 Inspect

```
进程级：
  - 角色、版本、启动时间、健康状态
  - 当前负载、队列长度、路由摘要

运行时级：
  - entity count
  - witness / ghost 概况
  - migration backlog
  - archiver / backup / session 状态

业务级：
  - 登录挑战配置
  - 封禁状态
  - 指定 entity 的只读检查
```

### 2.2 Health Probe 与状态公开

探活要拆成三类：

```
startup probe
  - 进程是否完成启动
  - script / defs / route / listener 是否完成初始化

liveness probe
  - tick 主循环是否还在前进
  - 关键线程是否卡死
  - 是否进入 fatal self-deadlock

readiness probe
  - 当前是否接受新流量 / 新登录 / 新实体迁入
  - 是否处于 draining / retiring / admitted overload
  - 依赖是否满足最小可服务条件
```

对外暴露的运行状态也应分级：

```
公开摘要：
  - role / version / uptime / process state
  - ready / live / startup 状态
  - draining / overload / route-ready 摘要

受控详情：
  - 完整状态树
  - entity / session / backup / migration 细节
  - profiler 采样与诊断产物
```

要求：

```
1. 探活接口可以被平台匿名或内网调用
2. 完整状态树不能匿名公开
3. readiness 失败不等于进程必须重启
4. liveness 失败才意味着应被 supervisor 拉起
```

### 2.3 Command

```
只允许幂等或受控命令：
  - kick session
  - set draining
  - retire process
  - controlled shutdown
  - clear transient bans
  - reload sampled config
```

### 2.4 Config

```
允许在线变更的配置必须满足：
  - 影响范围明确
  - 有审计记录
  - 有回滚入口
  - 不破坏协议和持久化兼容
```

---

## 3. 架构

```
Ops Console / CLI
  → AuthN / AuthZ
  → Ops Control Gateway
  → Target Resolver
  → Per-Process Ops Agent
  → Runtime Adapter
```

各层职责：

```
Ops Console
  - 人类或自动化入口

Ops Control Gateway
  - 权限校验
  - 请求审计
  - 限流与超时

Target Resolver
  - 根据 realm / role / process / entity 解析目标

Ops Agent
  - 与进程内状态树和命令注册表对接
```

探活和状态公开的典型入口：

```
/health/startup
/health/live
/health/ready
/status/summary
```

其中：

```
/health/*
  - 面向调度器 / supervisor / ingress

/status/summary
  - 面向运维面板 / CLI / 自动化平台
```

---

## 4. 统一对象模型

### 4.1 状态树

```cpp
// ops/IOpsNode.h

class IOpsNode {
public:
    virtual ~IOpsNode() = default;

    virtual std::string name() const = 0;
    virtual std::vector<std::string> children() const = 0;
    virtual JsonValue read(const std::string& path) const = 0;
};
```

### 4.2 命令接口

```cpp
// ops/IOpsCommand.h

struct OpsCommandContext {
    std::string operatorId;
    std::string requestId;
    std::string sourceIp;
};

class IOpsCommand {
public:
    virtual ~IOpsCommand() = default;

    virtual std::string name() const = 0;
    virtual OpsResult execute(const JsonValue& args,
                              const OpsCommandContext& ctx) = 0;
};
```

### 4.3 Agent 接口

```cpp
// ops/IOpsEndpoint.h

class IOpsEndpoint {
public:
    virtual ~IOpsEndpoint() = default;

    virtual Future<JsonValue> inspect(const std::string& path) = 0;
    virtual Future<OpsResult> call(const std::string& command,
                                   const JsonValue& args,
                                   const OpsCommandContext& ctx) = 0;
};
```

---

## 5. 和 Runtime / MessageBus / OTel 的关系

| 平面 | 作用 | 典型流量 |
|------|------|---------|
| Runtime Data Plane | 实体主路径 | EntityCall、ghost sync、迁移快照 |
| Control Plane | 进程注册、路由、生命周期协调 | 上下线、重发布、drain |
| Ops Control Plane | 观察、命令、在线调整 | inspect、retire、statusCheck |
| OTel Telemetry | traces / metrics / logs | span、metric、structured log |

要求：

```
1. Ops Control Plane 不承载高频实体数据流
2. OTel 不负责执行控制命令
3. Runtime 失败不应拖垮审计与命令权限边界
4. 探活只暴露摘要状态，不替代完整诊断
```

---

## 6. 安全模型

### 6.1 权限分级

```
ReadOnly
  - inspect process / metrics summary / config snapshot

Operator
  - kick session / set draining / clear temporary bans

Admin
  - controlled shutdown / retire process / apply runtime config
```

### 6.2 审计要求

所有写操作必须记录：

```
  - operatorId
  - requestId
  - target
  - command
  - args digest
  - result
  - timestamp
```

### 6.3 配置热改限制

禁止通过 Ops Control Plane 在线修改：

```
  - 协议定义
  - 持久化 schema
  - entity property flags
  - 迁移语义
```

---

## 7. Profiling 与 Watcher 的归位

BigWorld 的实体级 / 类型级 profiler 不应只被当成 metrics。

theseed 建议这样拆：

```
OTel:
  - 指标时间序列
  - trace 和日志

Ops Control Plane:
  - 按 entity / entity type / process 查询当前剖面
  - 按需触发一次采样
  - 查询退役、drain、迁移 backlog
```

火焰图这类诊断产物的归属是：

```
采样/导出语义
  - 见 05-telemetry-and-debug

触发/下载/鉴权入口
  - 见本篇 Ops Control Plane
```

这样既保留云原生 Telemetry，又不丢 BigWorld 式运维控制面。

---

## 8. 分阶段边界

```
MVP：
  - 只读 inspect
  - 少量受控命令：statusCheck / kick / draining
  - 操作审计

Phase 2：
  - 转发聚合
  - profiler / entity-type diagnostics
  - 更完整的登录与会话运维命令

Phase 3：
  - 跨 realm 运维代理
  - 与发布、热更、数据运维平台联动
```

落地对照（theseed 仓库，2026-09-26 实测口径）：

```
MVP 已落地：
  - 只读 inspect：machine.snapshot / machine.audit / 进程枚举 /
    剖面清单与下载（agent 侧 + 中心侧），≥ReadOnly；
  - 少量受控命令：machine.kick-session（≙ kick，吊销 SessionStore
    会话）、machine.set-draining（≙ draining，快照与上报同源透出）
    ——≥Operator；machine.shutdown（≙ §6.1 controlled shutdown，
    应答出站后 tick 末优雅停机）——≥Admin；
  - 操作审计：全部尝试（含拒绝）入 daemon 本地环形 + 中心聚合；
    §6.2 七字段全对齐——requestId 由 RuntimeInvocation 帧携带
    （u64，0 = 未携带），逐请求透传入审计条目与 machine.audit JSON
    的 request_id 字段。

MVP 内未落地（如实降级）：
  - statusCheck 未单独建：machine.snapshot 即只读探活语义；
  - clear temporary bans：仓库无临时封禁存储，前置缺失不造
    （§6.1 档位留档，见 todo.md 边界）。

Phase 2 已落地切片（会话运维命令面，2026-09-26）：
  - machine.list-sessions（≥ReadOnly）：经 SessionStore 二级索引枚举
    活会话，行内令牌只回显长度指纹（原文零泄露），账号/领域/user id
    供运维圈选；
  - machine.kick-sessions（≥Operator，≙ 批量 kick）：作用域选择器
    all | account=<id> | realm=<id>（指纹不可逆，不按令牌列表批量），
    逐个吊销，revoked/missing 分列回给调用方；部分失败仍入审计；
  - machine.extend-sessions（≥Operator，≙ 会话续期策略）：同款作用域
    选择器，把选中会话 refresh 到策略 TTL（Config.sessionRenewalTtl，
    策略值非调用方临时指定，扩策略须改配置评审——与 §6.3 白名单同
    纪律；0 = 入口关闭视同未知方法）；extended/missing 分列；
  - 踢人联动通知（≙ kick 联动）：单踢与批量踢吊销成功后向
    Config.sessionNotifyComponent 推送 machine.session.revoked（载荷
    {account, realm, session=指纹, reason=operator.kick |
    operator.kick.batch}）；best-effort——无重试/无确认，目标未注册
    即丢弃告警（吊销事实以存储为准，重连后可经 list-sessions 对账）；
    令牌原文不出进程。
  - 通知的生产接线（2026-09-27，闭环）：LoginApp 出站连 daemon 并发一条
    machine.snapshot 注册探针自报身份（daemon hub 沿用 attachServerTransport
    首请求自报注册，与 DBApp 同机制——不新增注册协议），tick 排空入站
    RuntimeInvocation 交分发面：machine.session.revoked 解载荷后调
    handleSessionRevoked（按 account+realm 关闭匹配的活跃登录连接，
    绑定表随断连清扫同步出表）；探针应答（snapshot.ok / machine.error）
    记联动状态，其余 method 记日志与计数不静默丢弃。载荷解析与 control 侧
    escapeJsonString 转义集严格对齐，越集转义按畸形拒收。
  - 会话行的领域口径修正：登录只写基础会话（realm 空），选领域时把 realm
    写回同一行（先 load 再改写，过期行不复活）。此前两侧口径不一致——
    通知的 realm 取自存储行、绑定的 realm 取自本地选择，联动对真实
    "登录后已选领域"的客户端会漏关；同时 list-sessions 的 realm 字段
    恒为空。修正后 list-sessions 的领域与踢人通知同源可用。
  - 通知腿运行期韧性（2026-09-27，闭环）：LoginApp 在 tick 里监督出站
    腿——活性以 transport 实况（IRuntimeTransport::isConnected，对端 EOF
    由 socket 泵转为假；TCP 层 30s 心跳使无 EOF 的半开也随读写错误收敛，
    内存实现缺省为真）+ daemon 方向任一入站证实（探针应答或推送皆可，
    注册在传输层与策略门独立）。断链或探针应答超窗（machineProbeAckTimeout
    ——connect 返回不作数，Linux 非阻塞 connect 恒 EINPROGRESS）即摘除
    死 peer、按指数退避重连（machineReconnectBaseDelay 起步、翻倍封顶
    machineReconnectMaxDelay、恢复复位）并重发 machine.snapshot 注册
    探针，daemon 侧 hub 经新连接自报重新注册订阅——daemon 重启后
    LoginApp 无需人工重启，吊销通知继续送达（真实 TCP 端到端可证）。
    断连期间照旧 best-effort：只 logWarn + 计数
    （login_machine_link_{down,up}_count），不阻断登录面。

Phase 2 仍开放：
  - 转发聚合 / entity-type diagnostics；
  - 更完整的登录与会话运维命令其余部分（clear temporary bans 仍因
    无封禁存储前置不做，沿既有边界）；
  - 断链期间漏送的通知不补发（无对账协议）——重连恢复的是订阅而非
    历史事实；吊销以存储为准，运维经 list-sessions 对账。db 腿传输
    仍为静态单次连接（同款监督属同性质扩面，未做）。
```

---

## 9. 与 BigWorld / KBEngine 的对比

| 维度 | BigWorld | KBEngine | theseed |
|------|---------|---------|---------|
| 分布式观察 | Watcher + ForwardingWatcher | Watcher 较弱 | 独立 Ops Control Plane |
| 在线命令 | 内建较丰富 | 较弱 | 命令注册 + 审计 |
| 实体级剖面 | 有 | 较弱 | OTel + Ops 查询分层 |
| 配置热改 | 局部支持 | 局部支持 | 显式权限与回滚边界 |
