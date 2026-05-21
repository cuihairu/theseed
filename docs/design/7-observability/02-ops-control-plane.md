# Ops Control Plane — 运维控制面与分布式观察

> 来源：BigWorld `Watcher / ForwardingWatcher / EntityProfiler / challenge watcher / controlled shutdown`。
> 这层不是 OTel，也不是 Runtime Data Plane，而是在线运维与控制的独立平面。

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

### 2.2 Command

```
只允许幂等或受控命令：
  - kick session
  - set draining
  - retire process
  - controlled shutdown
  - clear transient bans
  - reload sampled config
```

### 2.3 Config

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

---

## 9. 与 BigWorld / KBEngine 的对比

| 维度 | BigWorld | KBEngine | theseed |
|------|---------|---------|---------|
| 分布式观察 | Watcher + ForwardingWatcher | Watcher 较弱 | 独立 Ops Control Plane |
| 在线命令 | 内建较丰富 | 较弱 | 命令注册 + 审计 |
| 实体级剖面 | 有 | 较弱 | OTel + Ops 查询分层 |
| 配置热改 | 局部支持 | 局部支持 | 显式权限与回滚边界 |
