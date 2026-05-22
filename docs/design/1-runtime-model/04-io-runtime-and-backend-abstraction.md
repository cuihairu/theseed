# IO Runtime & Backend Abstraction — 跨平台 I/O 运行时与后端抽象

> 目标平台：Linux / Windows / macOS。
> 目标不是把 `epoll / kqueue / IOCP / io_uring` 混成一个最低公共分母，
> 而是提供一层足够高的运行时抽象，让 Tick、Transport、Gateway、控制面都可以复用同一套 I/O 生命周期。

---

## 0.5 引擎实现对照与取舍

### BigWorld 是怎么实现的

```
BigWorld 主要是 Mercury::EventDispatcher + NetworkInterface：
  - 事件循环和网络接口强绑定
  - 文件描述符注册模型清晰
  - 更接近传统 Reactor 风格

它的优势在于成熟稳定，
但不是按今天的 Reactor / Completion 分层来建模。
```

### KBEngine 是怎么实现的

```
KBEngine 主要是 EventDispatcher -> EventPoller -> poller_*：
  - Linux 下 epoll
  - 兜底 select
  - Windows 侧已有 IOCP 方向尝试

它比 BigWorld 更显式地把 poller 抽出来，
但顶层抽象仍然偏“fd readiness poller”，
不适合作为 io_uring / IOCP 的总抽象。
```

### 优缺点

```
BigWorld / KBEngine 的优点：
  - 事件循环简单直接
  - 网络主路径容易理解
  - 与单线程 tick 配合自然

共同缺点：
  - 顶层抽象过于贴近 select/epoll 时代
  - fd/register/deregister 不是 io_uring / IOCP 的理想语义
  - timer/task/wakeup/completion 没被统一抽象成同一运行时
```

### theseed 的取舍

```
theseed 不再把 EventPoller 当总抽象，
而是拆成三层：

  1. IORuntime
     统一 run / wakeup / submit / cancel / drainCompletions

  2. Backend Family
     - ReactorBackend: select / epoll / kqueue
     - CompletionBackend: io_uring / IOCP

  3. Transport Adapter
     TCP / UDP / WebSocket / Runtime Channel 在上层复用

这允许我们：
  - 保留 BigWorld / KBEngine 的 tick 友好主循环
  - 又不把现代完成式后端硬塞进 poller 语义
```

---

## 0. 设计边界

本篇负责：

```
  - Linux / Windows / macOS 的 I/O 后端抽象
  - Reactor 与 Completion 两类后端语义
  - 启动期能力探测与配置决策
  - Tick Runtime 与 I/O Runtime 的衔接
  - handle / request / completion / cancel 的统一接口
```

本篇不负责：

```
  - EntityCall / Mailbox 语义
  - Runtime Data Plane 的业务可靠性分级
  - Gateway / Login 的业务状态机
  - MessageBus 控制面
```

相关主题见：

```
../2-replication-and-space/03-runtime-communication-and-transport
../5-access-and-control-plane/01-gateway-and-login
../5-access-and-control-plane/02-message-bus-and-cross-realm
```

---

## 1. 为什么不能继续叫 EventPoller

`EventPoller` 只适合表达：

```
  - register fd for read
  - register fd for write
  - wait until ready
  - 回调处理
```

这套心智模型对：

```
select
epoll
kqueue
```

是自然的。

但对：

```
io_uring
IOCP
```

就不自然，因为它们更接近：

```
  - submit operation
  - operation completes later
  - consume completion
  - optionally cancel
```

所以如果继续把总抽象命名为 `EventPoller`，会出现三个问题：

```
1. 顶层接口被 fd/register 语义绑死
2. io_uring / IOCP 只能被“伪装成 readiness”
3. timer/task/wakeup/completion 无法并入同一运行时
```

结论：

```
EventPoller 可以保留为某一类后端的实现名，
但不能继续做 theseed 的总抽象。
```

---

## 2. 推荐的总抽象

总抽象命名建议：

```
IORuntime
```

它表达的是：

```
“驱动 I/O 请求、等待事件或完成、投递结果给上层 Tick 的运行时”
```

而不是：

```
“某种 fd poller”
```

### 2.1 核心接口

```cpp
enum class IoOp : uint8_t {
    Accept,
    Connect,
    Read,
    Write,
    RecvFrom,
    SendTo,
};

struct IoHandle {
    uint64_t value = 0;
    uint32_t generation = 0;
};

struct IoBuffer {
    void* data = nullptr;
    uint32_t size = 0;
};

struct IoRequest {
    IoOp op;
    IoHandle handle;
    IoBuffer buffer;
    void* userData = nullptr;
};

struct IoToken {
    uint64_t value = 0;
};

enum class IoStatus : uint8_t {
    Ok,
    Cancelled,
    Timeout,
    Closed,
    Error,
};

struct IoCompletion {
    IoToken token;
    IoStatus status = IoStatus::Ok;
    uint32_t bytesTransferred = 0;
    int32_t osError = 0;
    void* userData = nullptr;
};

class IIORuntime {
public:
    virtual ~IIORuntime() = default;

    virtual void runOnce(Duration maxWait) = 0;
    virtual void wakeup() = 0;

    virtual IoToken submit(const IoRequest& request) = 0;
    virtual bool cancel(IoToken token) = 0;

    virtual size_t drainCompletions(IoCompletion* out, size_t capacity) = 0;
};
```

这里最重要的变化是：

```
统一语义是 submit / cancel / completion，
不是 registerRead / registerWrite / triggerRead。
```

---

## 3. 两类后端家族

### 3.1 ReactorBackend

适用：

```
  - select
  - epoll
  - kqueue
```

语义：

```
  - 后端报告“句柄已就绪”
  - runtime 再把“就绪”翻译成可继续推进的 IoRequest
```

### 3.2 CompletionBackend

适用：

```
  - io_uring
  - IOCP
```

语义：

```
  - 先提交具体 I/O 操作
  - 后端直接产出完成事件
  - runtime 消费 completion queue
```

### 3.3 为什么不用写死 Proactor

严格说：

```
IOCP 更典型地接近 Proactor
io_uring 既能做 poll，也能做 completion
```

所以文档里更准确的叫法应是：

```
Reactor / Completion
```

而不是过度教条地只写：

```
Reactor / Proactor
```

---

## 4. 启动期后端选择

后端选择只允许发生在启动期，不允许运行时热切换。

### 4.1 选择流程

```
1. 探测系统能力
2. 读取配置策略
3. 选择最优后端
4. 输出最终决策日志
5. 初始化对应 runtime
```

### 4.2 能力探测

必须探测：

```
  - OS 类型
  - kernel / API 是否支持目标后端
  - 当前权限或资源限制是否允许启用
```

示例：

```
Linux:
  - 先探测 io_uring
  - 不可用则退回 epoll

macOS:
  - kqueue

Windows:
  - IOCP
```

### 4.3 配置策略

```toml
[runtime.io]
backend = "auto"
# auto
# force:io_uring
# force:epoll
# force:kqueue
# force:iocp
```

规则：

```
auto:
  自动选最优可用后端

force:*:
  必须启用指定后端
  若系统不支持，启动失败
```

### 4.4 选择接口

```cpp
enum class IoBackendKind : uint8_t {
    Select,
    Epoll,
    Kqueue,
    IoUring,
    Iocp,
};

struct IoBackendCapability {
    bool supported = false;
    std::string reason;
};

class IIORuntimeFactory {
public:
    virtual IoBackendCapability probe(IoBackendKind kind) = 0;
    virtual std::unique_ptr<IIORuntime> create(
        IoBackendKind kind,
        const RuntimeConfig& config) = 0;
};
```

---

## 5. 与 Tick 的关系

theseed 仍然保留：

```
单线程 Tick Runtime
```

I/O Runtime 不是另起一套业务线程模型，而是：

```
为 Tick 的 Network 阶段提供输入和完成事件
```

### 5.1 Tick 侧约束

```
1. 所有 completion 只在 owning tick thread 消费
2. 后台系统线程可以存在，但不能直接改 Entity
3. completion 必须先入 runtime queue，再进 Network phase
```

### 5.2 推荐时序

```text
Tick::NetworkPhase
  ├─ ioRuntime.runOnce(maxWait=0)
  ├─ ioRuntime.drainCompletions(...)
  ├─ translate completion -> packet / channel event
  └─ dispatch to runtime message queue
```

如果某些后端内部需要辅助线程：

```
允许存在，
但这些线程不拥有 Entity，也不直接进入脚本层。
```

---

## 6. Handle / Request / Completion 的分离

这层抽象必须避免把 `fd` 暴露成上层核心概念。

### 6.1 为什么不能只暴露 fd

```
1. Windows 不天然等价于 Unix fd 语义
2. IOCP / io_uring 的重点不在 fd readiness
3. 后续如果有 TLS、KCP、WebSocket 封装，逻辑句柄更稳定
```

### 6.2 推荐对象层次

```
IoHandle
  表示运行时管理的底层句柄身份

IoRequest
  表示一次待执行的 I/O 操作

IoCompletion
  表示一次已完成的 I/O 结果
```

### 6.3 取消语义

必须明确：

```
cancel(token) 只保证“尽力取消”
不保证调用返回后一定不会再收到 completion
```

因此上层必须接受：

```
completion 可能晚到
completion 必须携带 token / generation
上层按 epoch / handle generation 做幂等过滤
```

---

## 7. 与 Transport 的衔接

Transport 不应直接依赖具体后端，
而应依赖：

```
IIORuntime
```

### 7.1 传输适配层

```cpp
class ITransportEndpoint {
public:
    virtual ~ITransportEndpoint() = default;

    virtual void start(IIORuntime& ioRuntime) = 0;
    virtual void stop() = 0;

    virtual void enqueueSend(PacketSpan packet) = 0;
    virtual void onCompletion(const IoCompletion& completion) = 0;
};
```

这样：

```
TCP / UDP / WebSocket / Runtime Channel
```

都只面对统一 completion 语义。

### 7.2 旧式 poller 的位置

如果需要兼容旧风格实现，可以保留：

```cpp
class IReactorBackend {
public:
    virtual bool watchRead(IoHandle handle) = 0;
    virtual bool watchWrite(IoHandle handle) = 0;
    virtual void unwatchRead(IoHandle handle) = 0;
    virtual void unwatchWrite(IoHandle handle) = 0;
};
```

但它只属于：

```
ReactorBackend 内部
```

不能再向上传播成总接口。

---

## 8. MVP 边界

MVP 建议支持：

```
Linux:
  - epoll 必做
  - io_uring 作为 Phase 2 或实验特性

Windows:
  - IOCP 设计边界先立住
  - 实现优先级晚于 Linux epoll 主路径

macOS:
  - kqueue 设计边界先立住
  - 实现优先级晚于 Linux epoll 主路径
```

这样做的原因是：

```
1. MVP 先保证主平台可跑
2. 文档层先把长期抽象立住
3. 不把 io_uring 早期实现风险扩散进所有上层模块
```

---

## 9. 与 BigWorld / KBEngine 的最终关系

### BigWorld

```
可借鉴：
  - EventDispatcher 和主循环的稳定边界
  - NetworkInterface 与上层业务的清晰衔接

不直接照搬：
  - 老式 dispatcher 直接作为总抽象
```

### KBEngine

```
可借鉴：
  - EventDispatcher -> Poller 的显式拆层
  - Linux epoll / fallback select 的工程现实

不直接照搬：
  - 用 poller 语义覆盖 IOCP / io_uring
```

### theseed

```
theseed 的新增点不是“发明一种新 socket API”，
而是把老引擎的事件循环经验，
升级成：

  - 上层统一 IORuntime
  - 中层 Reactor / Completion 家族
  - 下层平台后端选择
```

这才适合 Linux / Windows / macOS 的长期支持目标。
