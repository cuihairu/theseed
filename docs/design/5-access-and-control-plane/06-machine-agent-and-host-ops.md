# MachineAgent & Host Ops - 主机上报、进程编排与节点摘要

> 来源源头：BigWorld `bwmachined`。
> 参考实现：KBEngine `machine` 的组件调度与状态汇聚路径。
> 这一层是“主机代理 + 本机进程编排 + 节点摘要上报”，不是完整监控中心。

---

## 0.5 引擎实现对照与取舍

### BigWorld 是怎么实现的

```
BigWorld 的 bwmachined 更像节点守护进程：
  - 维护机器与进程表
  - 做平台检查、版本检查、core dump 检查
  - 负责启动 / 停止 / 通知 / 发现
  - 还承担一定的集群侧机器公告和状态维护
```

### KBEngine 是怎么实现的

```
KBEngine 的 machine 更像组件调度与状态汇聚点：
  - 组件注册与查询
  - componentID 分配
  - 进程启动 / 停止 / kill
  - CPU / memory 采样与上报
```

### 优缺点

```
BigWorld 的优点：
  - 节点守护职责完整
  - 对机器和进程状态的覆盖更系统

BigWorld 的缺点：
  - 容易把机器代理做得过重
  - 需要更严格地区分控制面和观测面

KBEngine 的优点：
  - machine 语义更直接
  - 更贴近组件编排入口

KBEngine 的缺点：
  - 监控和控制边界仍然偏粗
  - 资源采样更多是点状能力
```

### theseed 的取舍

```
theseed 保留一个 MachineAgent，
但不把它做成全功能监控中心。

它只负责：
  - 主机资源摘要
  - 本机进程编排
  - 健康状态回传
  - 受控的节点级命令执行
```

---

## 0. 设计边界

本篇负责：

```
  - CPU / memory / disk / network 等主机摘要采样
  - 本机进程表、端口、版本、二进制存在性等信息
  - spawn / stop / restart / drain / retire 之类的本机动作
  - 向 Ops Control Plane 上报节点摘要
```

本篇不负责：

```
  - 长期指标存储
  - 指标聚合和告警规则
  - trace / flamegraph / profile 的存储与分析
  - 跨集群的全局监控中心
```

它和其他文档的关系：

```
05-telemetry-and-debug
  - 负责详细指标、日志、trace、flamegraph

04-ops-control-plane
  - 负责节点摘要查询、受控命令、审计

03-cluster-lifecycle
  - 负责 drain / retire / controlled shutdown 的状态推进
```

---

## 1. 为什么 machine 不能变成监控中心

如果把 machine 做成监控中心，会同时吞掉三类职责：

```
1. 观测采集
2. 运维控制
3. 状态汇总
```

这会导致边界失真：

```
  - 采样频率和控制命令互相污染
  - 历史存储和本地代理耦合
  - 机器级职责开始覆盖集群级职责
```

更合理的拆法是：

```
MachineAgent
  - 采样和上报当前节点状态
  - 执行本机级动作

Ops Control Plane
  - 聚合多个节点摘要
  - 提供运维查询与命令入口

Telemetry
  - 保留高频、细粒度、可追踪的诊断数据
```

所以 machine 可以像一个简易监控中心，
但本质上只应提供：

```
NodeSummary / HostSummary / ProcessSummary
```

而不是一个真正的监控后端。

---

## 2. MachineAgent 的职责

### 2.1 主机摘要

建议最少包含：

```
  - host id / hostname / platform
  - cpu 使用率
  - memory 总量 / 已用 / 可用
  - disk 容量 / 剩余 / 压力标记
  - network 基础吞吐摘要
  - load average 或等价指标
```

### 2.2 进程摘要

```
  - pid / component type / component id
  - binary path / version / start time
  - 端口占用情况
  - 运行状态 / 健康状态
  - core dump / crash 标记
```

### 2.3 本机编排

```
  - 启动进程
  - 停止进程
  - 重启进程
  - 触发 drain
  - 协助 retire / controlled shutdown
```

### 2.4 节点摘要上报

```
  - 周期性上报 MachineSnapshot
  - 事件型上报 process birth / death / crash
  - 把高频细节留给 Telemetry，把低频摘要留给 Ops
```

---

## 3. 推荐的数据模型

```cpp
struct HostSummary {
    std::string hostname;
    double cpuUsage = 0.0;
    double memoryUsage = 0.0;
    double diskUsage = 0.0;
    double loadAverage = 0.0;
    uint64_t networkRxBytes = 0;
    uint64_t networkTxBytes = 0;
};

struct ProcessSummary {
    std::string name;
    uint32_t pid = 0;
    uint32_t port = 0;
    std::string version;
    bool healthy = false;
};

struct NodeSummary {
    HostSummary host;
    std::vector<ProcessSummary> processes;
    bool draining = false;
    bool overloaded = false;
};
```

这些结构体的原则是：

```
  - 面向摘要，不面向完整诊断
  - 面向当前状态，不面向历史时序
  - 面向控制面消费，不面向直接持久化
```

---

## 4. 接口边界

### 4.1 采样接口

```cpp
class IHostProbe {
public:
    virtual ~IHostProbe() = default;
    virtual HostSummary sample() = 0;
};
```

### 4.2 进程管理接口

```cpp
class IProcessSupervisor {
public:
    virtual ~IProcessSupervisor() = default;
    virtual bool start(const std::string& target) = 0;
    virtual bool stop(uint32_t pid) = 0;
    virtual bool restart(uint32_t pid) = 0;
};
```

### 4.3 节点代理接口

```cpp
class IMachineAgent {
public:
    virtual ~IMachineAgent() = default;
    virtual NodeSummary snapshot() = 0;
    virtual void report() = 0;
    virtual bool execute(const std::string& command, const JsonValue& args) = 0;
};
```

---

## 5. 与 BigWorld / KBEngine 的对照

### BigWorld

```
bwmachined 的特点：
  - 更像机器级守护和集群入口
  - 既负责发现，也负责启动和通知
  - 还会处理一些平台检查和状态维护
```

### KBEngine

```
machine 的特点：
  - 更像组件调度和状态汇聚入口
  - CPU / memory 采样已经进入职责范围
  - 与 component 注册和查询直接绑定
```

### theseed

```
theseed 收敛成单一 MachineAgent：
  - 只做主机级摘要和本机编排
  - 全局运维视图放到 Ops Control Plane
  - 详细诊断放到 Telemetry
```

---

## 6. 简易监控中心的正确打开方式

如果确实需要像一个简易监控中心的角色，
建议做成下面这条链路：

```
MachineAgent
  -> 上报 NodeSummary
  -> Ops Control Plane 聚合
  -> /status/summary 对外展示
```

这样可以得到：

```
  - 节点健康总览
  - 资源压力摘要
  - 进程在线状态
  - drain / retire / overload 标记
```

但不会把 machine 变成：

```
  - Grafana 替代品
  - Metrics 存储后端
  - Trace 存储后端
  - 告警编排中心
```

---

## 7. 分阶段边界

### MVP

```
  - host cpu / memory / disk 摘要
  - process list / state / pid
  - 本机 start / stop / restart
  - 节点级 health summary
```

### Phase 2

```
  - core dump / crash 标记
  - 端口冲突 / binary version / platform probe
  - 更完整的 drain / retire 协调
```

### Phase 3

```
  - 更丰富的跨节点发现
  - 更细粒度的机器标签和策略
  - 与自动化运维策略更紧密的联动
```

---

## 8. 一句话判断

```
machine 应该是节点代理和本机编排入口，
不是全功能监控中心。
```
