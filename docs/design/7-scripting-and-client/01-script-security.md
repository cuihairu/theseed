# Script Security — 脚本层安全与稳定性

> 核心原则：**不防自己人，防客户端、防 bug、防泄露**。
> 游戏服务器脚本是开发者自己的业务代码，不需要沙箱白名单。
>
> 来源：KBEngine 无内建安全机制，theseed 新增完整防护。

---

## 0.5 引擎实现对照与取舍

### BigWorld 是怎么实现的

```
BigWorld 的脚本侧重点不是“安全体系”，而是：
  - Python 集成
  - 错误输出
  - watcher / reload 辅助
  - 运行时绑定
```

### KBEngine 是怎么实现的

```
KBEngine 也没有内建完整脚本安全层。
它更偏向：
  - Python 业务脚本
  - 运行时校验
  - 少量手工保护
```

### 优缺点

```
BigWorld / KBEngine 的优点：
  - 简单直接
  - 不增加太多脚本侧复杂度

BigWorld / KBEngine 的缺点：
  - 缺少源码保护
  - 缺少统一参数防线
  - 缺少脚本稳定性治理
```

### theseed 的取舍

```
theseed 选择把脚本安全单独抽层，
因为脚本不只是业务实现，还会成为对外暴露面和事故源。

代价是：
  - 构建链更复杂
  - 需要统一 def、频率、超时、异常保护
```

---

## 1. 源码保护

### 1.1 Python：.pyc 部署

```bash
# 构建流程
theseed-build compile-scripts --source scripts/ --output deploy/scripts/ --strip-source
# deploy/scripts/ 只包含 .pyc 字节码，不包含 .py 源文件
```

### 1.2 Lua：预编译字节码

```bash
theseed-build compile-scripts --source scripts/ --output deploy/scripts/
# 生成 .luac 字节码
```

### 1.3 保护层级

```
L1: .pyc / .luac 预编译部署     → 成本零，防直接拷贝源码
L2: 加密打包                     → AES 加密 + 引擎内存解密
L3: 自定义 opcode（Lua）          → 修改 Lua 虚拟机 opcode 映射
L4: 核心逻辑下沉 C++             → 脚本只做配置和调度
```

---

## 2. 客户端防线

### 2.1 三层防线

```
客户端请求 → [Gateway 限流] → [def 参数校验] → [脚本业务校验]
```

### 2.2 def 层声明防线

```xml
<Method name="attack" exposed="true" rateLimit="5/1s">
    <Arg name="targetId" type="ENTITY_ID" minValue="1"/>
    <Arg name="skillId" type="UINT32" minValue="1" maxValue="10000"/>
</Method>

<Method name="chat" exposed="true" rateLimit="3/1s">
    <Arg name="message" type="STRING" maxLength="200"/>
</Method>
```

### 2.3 频率限制

```cpp
class MethodRateLimiter {
public:
    bool check(EntityId entityId, const std::string& method,
               int maxCalls, int windowSeconds);
    // 滑动窗口计数器
};
```

---

## 3. 运行时韧性

> 不是"安全"，是"稳定性"。防的是 bug，不是攻击。

### 3.1 超时保护

```cpp
class ScriptGuard {
    ScriptGuard(const std::string& context, Duration timeout);
    ~ScriptGuard();  // 自动记录执行时间，超时告警
    bool checkTimeout() const;
};
```

### 3.2 异常捕获

```cpp
void ScriptEngine::executeSafe(const std::string& context, std::function<void()> fn) {
    try {
        fn();
    } catch (const ScriptException& e) {
        LOG_ERROR("script error", {{"context", context}, {"error", e.what()}});
        METRIC_COUNTER("theseed.script.error", 1);
        // 不崩溃，继续运行
    }
}
```

### 3.3 递归深度限制

```cpp
void ScriptEngine::executeMethod(EntityId id, const std::string& method) {
    int depth = ++callDepth_;
    if (depth > config_.maxRecursionDepth) {
        LOG_ERROR("recursion depth exceeded", ...);
        --callDepth_;
        return;
    }
    ScriptGuard guard(...);
    doExecute(id, method);
    --callDepth_;
}
```

脚本安全只管“别炸”和“别泄露”，
脚本调试见 `03-script-debug`，不要混为一谈。

---

## 4. 与 KBEngine 的对比

| 能力 | KBEngine | theseed |
|------|---------|---------|
| 源码保护 | .pyc（手动） | 构建工具自动编译 + 可选加密 |
| 客户端输入校验 | 运行时类型检查 | def 声明 + 自动生成校验代码 |
| 参数范围校验 | 无 | minValue/maxValue/maxLength |
| 频率限制 | 无 | per-entity per-method 滑动窗口 |
| 异常捕获 | 部分覆盖 | 100% 覆盖，脚本 bug 不崩溃 |
| 超时保护 | 无 | ScriptGuard 自动超时 |
| 递归保护 | Python 默认 | 自定义限制 + 监控 |
