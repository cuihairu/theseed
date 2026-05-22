# Script Debug — 脚本层调试与执行态诊断

> 本篇只讨论脚本运行时调试，不讨论平台遥测，也不讨论运维控制命令。
> BigWorld / KBEngine 的主线更多是 `PyErr_Print`、`traceback`、`watcher`、`reload`；
> theseed 在此之上单独补脚本执行态调试面。

---

## 0.5 引擎实现对照与取舍

### BigWorld 是怎么实现的

```
BigWorld 的脚本调试更偏基础能力：
  - PyErr_Print
  - traceback
  - watcher 辅助观察
  - reload
```

### KBEngine 是怎么实现的

```
KBEngine 也主要围绕：
  - traceback
  - reload
  - 简单观察与日志

并没有独立、完整的脚本执行态调试面。
```

### 优缺点

```
BigWorld / KBEngine 的优点：
  - 简单
  - 对运行时侵入较少

共同缺点：
  - 缺少断点、单步、局部变量和协程态调试
  - 复杂线上脚本问题定位效率低
```

### theseed 的取舍

```
theseed 把脚本调试独立成篇，
因为它既不是 Telemetry，
也不是 Ops Control Plane，
而是服务器脚本执行态能力。
```

---

## 0. 设计边界

本篇负责：

```
  - breakpoint / step / continue
  - call stack / locals / closure / watch
  - Entity / Space / Realm 上下文
  - coroutine / yield / resume
  - hot update 后的版本一致性校验
  - 调试事件审计与会话管理
```

本篇不负责：

```
  - traces / metrics / logs
  - watcher 状态树查询与在线命令
  - 热更 diff 分析与滚动发布编排
  - 业务安全校验或客户端授权入口
  - profiler → load feedback 闭环
```

脚本调试是服务器脚本执行态能力，
不是 OTel，也不是 Ops Control Plane。

---

## 1. 为什么要单独成篇

BigWorld / KBEngine 在脚本侧主要提供的是：

```
  - 异常打印
  - traceback
  - watcher 观察
  - reload / reload-like 机制
```

这些能力足够定位错误，但不等于一套成体系的脚本调试面。
theseed 需要补的是：

```
  - 断点
  - 单步
  - 栈与局部变量
  - Entity 上下文
  - yield/resume
  - 热更后版本校验
```

如果把这些继续混进 Telemetry，就会把“看见发生了什么”和“控制脚本怎么跑”混在一起。

---

## 2. 调试对象

典型调试对象包括：

```
  - 普通函数调用帧
  - Entity 绑定方法帧
  - 协程挂起点
  - 异步回调封装帧
  - reload 之后的旧版本帧
```

每个调试目标都必须携带最少上下文：

```
  - processId
  - entityId
  - entityEpoch
  - scriptVersion
  - scriptPath
  - sourceLine
```

---

## 3. 调试会话模型

```cpp
struct ScriptDebugTarget {
    ProcessId processId;
    EntityId entityId;
    uint64_t entityEpoch;
    uint64_t scriptVersion;
    std::string scriptPath;
};

struct ScriptFrameSnapshot {
    std::string functionName;
    std::string sourcePath;
    uint32_t line = 0;
    std::vector<std::string> locals;
};

class IScriptDebugSession {
public:
    virtual void attach(const ScriptDebugTarget& target) = 0;
    virtual void detach() = 0;
    virtual BreakpointId setBreakpoint(const std::string& path, uint32_t line) = 0;
    virtual void clearBreakpoint(BreakpointId id) = 0;
    virtual void stepInto() = 0;
    virtual void stepOver() = 0;
    virtual void stepOut() = 0;
    virtual void resume() = 0;
    virtual ScriptFrameSnapshot inspectFrame(uint32_t index) = 0;
};
```

协议层可以参考 DAP 风格，
但语义必须以 Entity / Realm / Version 为中心，
不能直接假设通用语言调试器语义可以无损套用。

---

## 4. 与热更和异步的关系

```
热更、迁移、异步回调，都会让旧帧失效。
```

因此脚本调试必须显式绑定：

```
  - entityEpoch
  - scriptVersion
  - suspensionId
```

规则很简单：

```
只要版本不一致，resume 就必须失败或重建上下文，
不能静默继续跑旧帧。
```

异步回调也一样：

```
迟到回调可以重新入栈，
但不能自动继承旧的暂停状态。
```

---

## 5. 与生命周期和控制器的关系

脚本调试必须把这些回调当作一等帧：

```
  - Entity 生命周期钩子
  - Controller onStart / onUpdate / onComplete / onCancel
  - 定时器回调
  - 异步回调
```

可暂停点只允许落在这些安全边界上：

```
  - 脚本函数入口
  - hook 回调入口
  - controller 回调入口
  - timer / future 回调入口
```

不允许把暂停点插进：

```
  - C++ 核心同步路径
  - 属性 flush 中途
  - 迁移序列化中途
```

---

## 6. 安全与权限

脚本调试只允许：

```
  - 开发态
  - 已认证运维态
  - 显式授权的本地会话
```

禁止：

```
  - 通过客户端 exposed 入口进入调试面
  - 把 locals / stack 直接暴露给普通玩家请求
  - 把调试权限和 Telemetry 读取权限混为一谈
```

调试会话必须审计：

```
  - attach
  - breakpoint change
  - step
  - resume
  - detach
```

---

## 7. 分阶段边界

```
MVP：
  - attach / detach
  - traceback / stack dump
  - Entity 上下文 inspect
  - 协程挂起点列表

Phase 2：
  - breakpoint / step / continue
  - watch expression
  - conditional breakpoint
  - yield/resume

Phase 3：
  - 远程调试适配器
  - 多进程联动调试
  - 热更感知的会话重绑定
```

---

## 8. 与 BigWorld / KBEngine / theseed 的对比

| 维度 | BigWorld / KBEngine | theseed |
|------|---------------------|---------|
| 脚本异常定位 | traceback / watcher / reload 辅助 | 独立 Script Debug |
| 断点 / 单步 | 无成体系 | 有 |
| Entity 上下文 | 分散 | 显式建模 |
| yield / resume | 基本无 | 显式建模 |
| 热更版本一致性 | 无 | 强制校验 |
