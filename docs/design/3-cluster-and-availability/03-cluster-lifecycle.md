# Cluster Lifecycle — Service Fragment、Retire、Drain 与受控停服

> 来源：BigWorld `ServiceApp / registerServiceFragment / retireApp / controlledShutDown`。
> 这层描述的是“集群生命周期面”，不是 Runtime 主路径，也不是单纯的容灾文档。

---

## 0.5 引擎实现对照与取舍

### BigWorld 是怎么实现的

```
BigWorld 有很完整的集群生命周期模型：
  - Service Fragment 注册
  - retire / drain
  - controlled shutdown
  - 多进程角色协同退出
```

### KBEngine 是怎么实现的

```
KBEngine 有组件生命周期管理，
但没有 BigWorld 那么完整的 retire / fragment 体系。
```

### 优缺点

```
BigWorld 的优点：
  - 运维动作语义化
  - 停服和迁出链条更可控

KBEngine 的优点：
  - 简单
  - 组件生命周期更直接

共同缺点：
  - 生命周期越完整，实现和验证越复杂
```

### theseed 的取舍

```
theseed 把 Cluster Lifecycle 单独成篇，
因为它属于 BigWorld 级系统层差异，
不能再埋在容错或运维零散章节里。
```

### 为什么不能并回容灾或 Ops

```
Cluster Lifecycle 如果并回 Fault Tolerance，
会把“恢复”和“在线退役/停服编排”混成一件事。

Cluster Lifecycle 如果并回 Ops，
又会把“执行入口”与“系统状态推进”混成一件事。

所以它必须独立：
  - Fault Tolerance 讲 crash 后怎么办
  - Ops 讲谁来发命令
  - Cluster Lifecycle 讲系统如何进入下一状态
```

---

## 0. 设计边界

```
Cluster Lifecycle 负责：
  - 进程进入 / 退出集群
  - drain / retire / shutdown 协调
  - service fragment 注册与存活约束
  - 多角色停服阶段推进

Cluster Lifecycle 不负责：
  - EntityCall 主路径
  - AOI / witness / ghost 同步
  - 主持久化接口
  - metrics / traces 存储
```

它与其他文档的关系：

```
01-fault-tolerance
  - 更偏恢复与容灾

本篇
  - 更偏在线生命周期协调与停服编排
```

---

## 1. 为什么这层必须独立

BigWorld 的系统层有一条非常明显的能力线：

```
1. BaseApp / ServiceApp 可退役
2. Service fragment 可注册 / 注销
3. 服务缺失可触发停服策略
4. controlled shutdown 有阶段推进
5. LoginApp / DBApp / BaseApp / CellAppMgr 都参与
```

如果不把这层单独立出来，文档会出现两个问题：

```
1. 把 drain / retire 当成部署脚本细节
2. 把 shutdown 当成“容灾补充说明”
```

实际上它是独立的集群控制面。

更直接地说：

```
drain / retire / controlled shutdown
不是部署脚本技巧，
而是引擎级状态机。
```

---

## 2. 核心概念

### 2.1 Drain

```
含义：
  - 进程停止接收新流量
  - 允许在途工作完成
  - 等待迁移、归档、会话转移
```

### 2.2 Retire

```
含义：
  - 进程准备退出集群
  - 必须先 drain
  - 必须处理残留 entity / session / service fragment
```

### 2.3 Controlled Shutdown

```
含义：
  - 集群协调的阶段化停服
  - 各角色按顺序执行
  - 不是简单 kill process
```

### 2.4 Service Fragment

```
含义：
  - 某个逻辑服务能力实例
  - 可部署在专用 ServiceApp 或特殊 BaseApp 上
  - 生命周期独立于普通玩家实体
```

---

## 3. Service Fragment 模型

### 3.1 职责

Service Fragment 用来承载：

```
  - 全局服务逻辑
  - 匹配 / 排行 / 邮件 / 活动调度
  - 不适合绑在普通玩家 Base 上的长生命周期能力
```

### 3.2 注册关系

```
Service Fragment Instance
  → belongs to Process
  → advertises ServiceName
  → registers to Cluster Lifecycle Registry
```

### 3.3 设计要求

```
1. 服务注册必须显式
2. 一个 service 可有多个 fragment
3. fragment 消失是否触发停服，必须是策略配置
4. 不能把“有这个实体”误当成“服务可用”
```

---

## 4. 核心接口

### 4.1 生命周期接口

```cpp
// cluster/IClusterLifecycleManager.h

enum class LifecycleState {
    Joining,
    Running,
    Draining,
    Retiring,
    ShuttingDown,
    Stopped
};

class IClusterLifecycleManager {
public:
    virtual ~IClusterLifecycleManager() = default;

    virtual Future<void> setDraining(ProcessId id, bool value) = 0;
    virtual Future<void> retire(ProcessId id) = 0;
    virtual Future<void> controlledShutdown(ShutdownStage stage) = 0;
    virtual LifecycleState state(ProcessId id) const = 0;
};
```

### 4.2 Service Registry

```cpp
// cluster/IServiceFragmentRegistry.h

struct ServiceFragmentInfo {
    std::string serviceName;
    ProcessId processId;
    FragmentId fragmentId;
    bool healthy = true;
};

class IServiceFragmentRegistry {
public:
    virtual ~IServiceFragmentRegistry() = default;

    virtual Future<void> registerFragment(
        const ServiceFragmentInfo& info) = 0;
    virtual Future<void> deregisterFragment(
        const std::string& serviceName,
        FragmentId fragmentId) = 0;

    virtual std::vector<ServiceFragmentInfo> list(
        const std::string& serviceName) const = 0;
};
```

---

## 5. 进程状态机

```
Joining
  → Running
  → Draining
  → Retiring
  → Stopped

异常分支：
Running → ShuttingDown
Draining → ShuttingDown
Retiring → ShuttingDown
```

约束：

```
1. Running 才允许承接新流量
2. Draining 后不能再接新会话 / 新实体分配
3. Retiring 前必须完成或转移关键状态
4. Service Fragment 注销必须在进程最终退出前完成
```

---

## 6. Controlled Shutdown 分阶段

建议 theseed 明确采用阶段化停服，而不是一个布尔开关。

```
Stage 1: Request
  - 广播停服意图
  - 拒绝新登录

Stage 2: Drain
  - Gateway / Login / Session 进入 draining
  - Base / Service 开始迁移与清理

Stage 3: Persist
  - Archiver / LocalArchiveStore flush
  - 关键会话与实体状态落盘

Stage 4: Perform
  - 停止进程主循环
  - 注销 fragment / 路由 / 注册信息

Stage 5: Complete
  - 全集群确认结束
```

要求：

```
1. 每个阶段都必须可观测
2. 任意阶段失败都要给出 operator 可见原因
3. 不允许跳过 Persist 直接粗暴退出
```

---

## 7. 与热更 / 发布的关系

滚动更新本质上是“受控退役 + 新实例加入”的重复过程。

因此：

```
rolling update
  = join new
  + drain old
  + retire old
```

不能把热更文档里的 `drain → migrate → restart` 当成局部技巧，它应该复用本篇生命周期模型。

---

## 8. 失效策略

### 8.1 服务缺失

对于 Service Fragment，必须支持策略化处理：

```yaml
cluster_lifecycle:
  service_failure_policy:
    mail_service: degrade
    match_service: degrade
    payment_callback: shutdown
```

三类策略：

```
ignore
  - 只告警，不停服

degrade
  - 标记服务不可用，业务降级

shutdown
  - 触发 controlled shutdown
```

### 8.2 退役失败

```
如果 drain 超时：
  - 允许 operator 继续等待
  - 允许强制进入 shutdown
  - 必须输出阻塞原因摘要
```

---

## 9. 和 Ops Control Plane 的关系

本篇定义的是生命周期模型，命令入口应走 `Ops Control Plane`。

对应关系：

| 生命周期动作 | 命令入口 |
|------|------|
| set draining | Ops Control Plane |
| retire process | Ops Control Plane |
| controlled shutdown | Ops Control Plane |
| inspect fragment registry | Ops Control Plane |

也就是说：

```
Lifecycle 定义语义
Ops Control Plane 提供入口与审计
```

---

## 10. 分阶段边界

```
MVP：
  - set draining
  - retire process
  - 基础 controlled shutdown
  - 不引入完整 ServiceApp 体系

Phase 2：
  - Service Fragment Registry
  - fragment failure policy
  - 与发布系统联动

Phase 3：
  - 更细粒度 shutdown stage
  - 跨 realm 生命周期编排
```

---

## 11. 与 BigWorld / KBEngine 的对比

| 维度 | BigWorld | KBEngine | theseed |
|------|---------|---------|---------|
| retireApp | 有 | 较弱 | 明确建模 |
| controlled shutdown | 有完整阶段 | 较弱 | 显式阶段化 |
| Service Fragment | 有 | 基本无 | Phase 2 目标 |
| service death policy | 有 | 较弱 | 显式策略配置 |
| rolling update 语义 | 有系统支撑 | 较弱 | 复用生命周期模型 |
