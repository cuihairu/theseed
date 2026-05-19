# Script Safety Design — 脚本层安全与稳定性

> theseed 脚本层设计与运维保障。
> 核心原则：**不防自己人，防客户端、防 bug、防泄露**。
>
> 游戏服务器脚本是开发者自己写的业务代码，不需要沙箱白名单。
> 真正需要的安全关注点：
> 1. 源码保护：部署到服务器后防止源码泄露
> 2. 客户端防线：exposed 方法的输入校验和频率限制
> 3. 运行时韧性：超时、异常、递归保护（防 bug 不是防攻击）
> 4. 热更验证：运维变更时的安全保障

---

## 1. 源码保护

### 1.1 问题场景

```
现实威胁：
  - 开发人员从服务器拷贝脚本源码带走
  - 运维人员无意泄露
  - 服务器被入侵后源码暴露

不是威胁：
  - 开发者自己调用 os.system —— 那是业务需要
  - 开发者 import socket —— 可能是做 HTTP 回调
  - 沙箱白名单 —— 自己的代码为什么要防自己？
```

### 1.2 Python：.pyc 部署

```bash
# 构建流程：源码 → .pyc 字节码
theseed-build compile-scripts --source scripts/ --output deploy/scripts/ --strip-source

# 效果：
#   deploy/scripts/
#   ├── game/
#   │   ├── Avatar.pyc      # 字节码
#   │   ├── Monster.pyc
#   │   └── __init__.pyc
#   └── lib/
#       └── ...

# 不包含任何 .py 源文件
# .pyc 是 Python 字节码，不是源码，反编译成本高
```

```cpp
// 引擎启动时只加载 .pyc，不加载 .py
void PythonBackend::loadScripts(const std::string& scriptPath) {
    // 1. 扫描目录，只认 .pyc 文件
    // 2. 如果同时存在 .py 和 .pyc，优先加载 .pyc（生产环境不应有 .py）
    // 3. 配置项：script.deployment = "compiled" 时，如果发现 .py 源文件则告警
}
```

```xml
<!-- config/deployment.xml -->
<deployment>
    <!-- compiled: 只加载 .pyc，发现 .py 源文件则告警 -->
    <!-- source: 加载 .py 源文件（开发环境） -->
    <python mode="compiled"/>
    <lua mode="compiled"/>
</deployment>
```

### 1.3 Lua：预编译字节码

```bash
# Lua 源码预编译
theseed-build compile-scripts --source scripts/ --output deploy/scripts/

# 使用 luac 编译为字节码
#   deploy/scripts/
#   ├── game/
#   │   ├── Avatar.luac     # Lua 字节码
#   │   └── Monster.luac

# 注意：Lua 字节码版本敏感，必须用对应版本的 luac 编译
# 反编译工具存在（luadec 等），但提高了门槛
# 如需更高保护，可用自定义 opcode 映射
```

### 1.4 高级保护（可选）

```
保护层级（按成本递增）：

L1: .pyc / .luac 预编译部署
    → 成本：零（Python/Lua 原生支持）
    → 防护：防止直接拷贝源码

L2: 加密打包
    → 脚本字节码加密后打包，引擎启动时内存解密
    → 防护：防止直接拷贝字节码文件
    → 实现：AES 加密 + 引擎内置解密

L3: 自定义 opcode（Lua）
    → 修改 Lua 虚拟机的 opcode 映射表
    → 标准反编译工具失效
    → 成本：维护自定义 Lua 构建

L4: 核心逻辑下沉 C++
    → 最关键的业务逻辑用 C++ 实现
    → 脚本只做配置和调度
    → 成本：开发效率降低
```

---

## 2. 客户端防线

### 2.1 威胁模型

```
客户端是黑客：
  - 发送伪造的 exposed 方法调用（非法参数、超范围值）
  - 高频刷接口（加速、刷金币）
  - 调用不该调用的方法（跳过权限检查）
  - 发送畸形数据（超长字符串、负数 ID）

客户端不是黑客：
  - 正常游戏操作
  - 合理的 exposed 方法调用
```

### 2.2 def 层声明防线

```xml
<!-- entities/Avatar/Avatar.def -->
<Entity name="Avatar" sides="base,cell" ...>

    <CellMethods>
        <!-- attack: 每秒最多 5 次，参数范围校验 -->
        <Method name="attack" exposed="true" rateLimit="5/1s">
            <Arg name="targetId" type="ENTITY_ID" minValue="1"/>
            <Arg name="skillId" type="UINT32" minValue="1" maxValue="10000"/>
        </Method>

        <!-- move: 高频但轻量，每秒 20 次 -->
        <Method name="move" exposed="true" rateLimit="20/1s">
            <Arg name="position" type="VECTOR3">
                <Range minX="-10000" maxX="10000"
                       minY="0" maxY="1000"
                       minZ="-10000" maxZ="10000"/>
            </Arg>
            <Arg name="rotation" type="VECTOR3">
                <Range minX="0" maxX="360" minY="0" maxY="360" minZ="0" maxZ="360"/>
            </Arg>
        </Method>

        <!-- chat: 每秒 3 次，字符串长度限制 -->
        <Method name="chat" exposed="true" rateLimit="3/1s">
            <Arg name="message" type="STRING" maxLength="200"/>
            <Arg name="channel" type="UINT32" minValue="0" maxValue="10"/>
        </Method>

        <!-- useItem: 频率低但关键 -->
        <Method name="useItem" exposed="true" rateLimit="10/1s">
            <Arg name="itemId" type="UINT32" minValue="1"/>
            <Arg name="count" type="UINT32" minValue="1" maxValue="999"/>
        </Method>
    </CellMethods>

</Entity>
```

### 2.3 三层防线实现

```
客户端请求 → [第一层: Gateway 限流] → [第二层: 类型+范围校验] → [第三层: 业务逻辑]
                  │                         │                         │
                  │ 全局频率限制              │ def 声明的参数校验       │ 脚本层业务检查
                  │ IP 级别                  │ 自动生成，无需手写       │ 权限、状态、逻辑
                  │ 在 Gateway 就拦截         │ 类型、范围、长度         │ 不可信任客户端传来的任何值
                  ↓                         ↓                         ↓
              直接拒绝                    拒绝 + 告警              正常处理
```

#### 第一层：Gateway 全局限流

```cpp
// gateway/RateLimiter.cpp

// 限制每个客户端连接的整体请求频率
// 超过限制直接在 Gateway 层拒绝，不进入引擎处理
class ConnectionRateLimiter {
    bool check(ClientId client, const std::string& method) {
        auto& counter = counters_[client];
        counter.increment();

        // 全局限制：每客户端每秒最多 100 次请求
        if (counter.rate() > 100) {
            LOG_WARN("connection rate limit", {
                {"client", client},
                {"method", method},
                {"rate", counter.rate()}
            });
            return false;
        }
        return true;
    }
};
```

#### 第二层：def 声明的参数校验（自动生成）

```cpp
// codegen 自动生成的校验代码
// 开发者不需要手写，由 def 文件驱动

void Avatar_exposed_attack_validate(EntityId targetId, uint32_t skillId) {
    // 类型校验：已在反序列化时完成

    // 范围校验
    if (targetId < 1) {
        throw ValidationException("attack.targetId: must >= 1");
    }
    if (skillId < 1 || skillId > 10000) {
        throw ValidationException("attack.skillId: must be in [1, 10000]");
    }
}
```

#### 第三层：脚本业务校验

```python
# 脚本层做业务逻辑校验
# 前两层已经保证了类型安全和频率安全

class Avatar(BaseEntity):
    def attack(self, targetId, skillId):
        # 业务校验：目标是否存在、是否在攻击范围、技能是否可用
        target = theseed.getEntity(targetId)
        if not target or not target.isAlive():
            return  # 目标不存在或已死亡

        if not self.isInRange(target, self.getSkillRange(skillId)):
            return  # 超出攻击范围

        if not self.canUseSkill(skillId):
            return  # 冷却中或蓝量不足

        # 执行攻击逻辑
        self.doAttack(target, skillId)
```

### 2.4 频率限制实现

```cpp
// script/RateLimiter.h

class MethodRateLimiter {
public:
    bool check(EntityId entityId, const std::string& method,
               int maxCalls, int windowSeconds) {
        auto key = std::make_pair(entityId, method);
        auto& counter = counters_[key];
        counter.increment();

        if (counter.countInWindow(windowSeconds) > maxCalls) {
            LOG_WARN("method rate limit exceeded", {
                {"entity", entityId},
                {"method", method},
                {"count", counter.countInWindow(windowSeconds)},
                {"limit", maxCalls}
            });
            return false;
        }
        return true;
    }

private:
    // 滑动窗口计数器
    std::map<std::pair<EntityId, std::string>, SlidingWindowCounter> counters_;
};
```

---

## 3. 运行时韧性

> 不是"安全"，是"稳定性"。防的是 bug，不是攻击。

### 3.1 超时保护

```cpp
// script/ScriptGuard.h

class ScriptGuard {
public:
    ScriptGuard(const std::string& context, Duration timeout)
        : context_(context)
        , deadline_(Clock::now() + timeout)
        , startTime_(Clock::now())
    {}

    ~ScriptGuard() {
        auto elapsed = Clock::now() - startTime_;
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

        // 记录执行时间
        METRIC_HISTOGRAM("theseed.script.exec_ms", elapsedMs,
                        {{"context", context_}});

        if (Clock::now() > deadline_) {
            LOG_WARN("script timeout", {
                {"context", context_},
                {"elapsed_ms", elapsedMs}
            });
        }
    }

    // 脚本内可调用，主动检查是否超时
    bool checkTimeout() const {
        return Clock::now() > deadline_;
    }

private:
    std::string context_;
    TimePoint deadline_;
    TimePoint startTime_;
};
```

### 3.2 异常捕获

```cpp
// 所有脚本调用都被 try-catch 包裹
// 游戏服务器不能因为一个脚本 bug 崩溃

void ScriptEngine::executeSafe(const std::string& context,
                                std::function<void()> fn) {
    try {
        fn();
    } catch (const ScriptException& e) {
        // 脚本异常（Python Exception / Lua Error）
        LOG_ERROR("script error", {
            {"context", context},
            {"error", e.what()},
            {"traceback", e.traceback()}
        });

        METRIC_COUNTER("theseed.script.error", 1, {
            {"context", context}
        });

        // 不崩溃，继续运行。脚本 bug 不应该导致服务器宕机。
    }
}
```

### 3.3 递归深度限制

```cpp
// 防止无限递归导致栈溢出
void ScriptEngine::executeMethod(EntityId id, const std::string& method) {
    int depth = ++callDepth_;

    if (depth > config_.maxRecursionDepth) {
        --callDepth_;
        LOG_ERROR("recursion depth exceeded", {
            {"entity", id},
            {"method", method},
            {"depth", depth}
        });
        return;
    }

    ScriptGuard guard(fmt::format("{}.{}", id, method), config_.maxScriptExecTime);
    doExecute(id, method);

    --callDepth_;
}
```

---

## 4. 热更验证

### 4.1 变更分级

```
热更新
├── L1: 配置热更（最安全）
│   ├── 游戏数值表
│   ├── 匹配规则
│   └── 运营活动配置
│
├── L2: 脚本热更（中等风险）
│   ├── 修改方法实现
│   ├── 添加新方法
│   └── 修改定时器逻辑
│
├── L3: 定义热更（高风险）
│   ├── 添加新属性（有默认值）
│   ├── 添加新 Entity 类型
│   └── 修改索引/约束
│
└── L4: 结构变更（不可热更，需滚动重启）
    ├── 删除属性
    ├── 修改属性类型
    └── 修改网络协议
```

### 4.2 热更验证流程

```python
# 热更前自动执行的验证流程

class HotUpdateValidator:
    def validate(self, old_defs, new_defs, old_scripts, new_scripts):
        results = []

        # 1. Schema 兼容性检查
        results.append(self._check_schema_compatibility(old_defs, new_defs))

        # 2. 接口兼容性检查（新增/删除/修改方法签名）
        results.append(self._check_interface_compatibility(old_defs, new_defs))

        # 3. 脚本静态分析
        results.append(self._static_analysis(new_scripts))

        # 4. 试运行（在隔离环境中执行新脚本的测试用例）
        results.append(self._dry_run(new_scripts))

        return ValidationResult.merge(results)

    def _check_schema_compatibility(self, old_defs, new_defs):
        """检查 def 变更是否兼容"""
        errors = []
        for entity_name, new_def in new_defs.items():
            old_def = old_defs.get(entity_name)
            if not old_def:
                continue  # 新增 entity，安全

            for prop_name, new_prop in new_def.properties.items():
                old_prop = old_def.properties.get(prop_name)
                if not old_prop:
                    continue  # 新增属性，需要有默认值

                if old_prop.type != new_prop.type:
                    errors.append(
                        f"INCOMPATIBLE: {entity_name}.{prop_name} type changed "
                        f"from {old_prop.type} to {new_prop.type}"
                    )

                if old_prop.persistent and not new_prop.persistent:
                    errors.append(
                        f"WARNING: {entity_name}.{prop_name} removed from persistent"
                    )

        return ValidationResult(errors)
```

### 4.3 变更审核配置

```xml
<!-- config/hotupdate-approval.xml -->
<hotUpdateApproval>

    <!-- 自动通过 -->
    <autoApprove>
        <rule changeType="add_property" hasDefault="true"/>
        <rule changeType="add_method"/>
        <rule changeType="modify_script_body"/>
    </autoApprove>

    <!-- 需要人工审核 -->
    <manualReview>
        <rule changeType="remove_property"/>
        <rule changeType="change_property_type"/>
        <rule changeType="modify_exposed_method_signature"/>
    </manualReview>

    <!-- 禁止热更，必须走滚动重启 -->
    <forbidden>
        <rule changeType="remove_entity_type"/>
        <rule changeType="change_network_protocol"/>
    </forbidden>

</hotUpdateApproval>
```

---

## 5. 审计与监控

### 5.1 安全事件日志

```json
{
  "event_type": "client_violation",
  "timestamp": "2026-05-19T14:23:45Z",
  "severity": "warning",
  "entity_id": 10042,
  "entity_type": "Avatar",
  "violation": {
    "type": "rate_limit_exceeded",
    "method": "attack",
    "count": 15,
    "limit": 5,
    "window": "1s",
    "client_ip": "203.0.113.42"
  },
  "action": "rejected"
}
```

### 5.2 监控指标

```
theseed_client_violation_total        # 客户端违规计数 (Counter)
  labels: type (rate_limit, validation, authorization)
  labels: entity_type, method

theseed_script_error_total            # 脚本错误计数 (Counter)
  labels: type (runtime, type_error, attribute_error)
  labels: entity_type

theseed_script_exec_ms                # 脚本执行耗时 (Histogram)
  labels: context

theseed_hotupdate_validation_result   # 热更验证结果 (Counter)
  labels: result (pass, warn, fail)
```

---

## 6. 与 KBEngine 的对比

| 能力 | KBEngine | theseed |
|------|---------|---------|
| **源码保护** | .pyc 部署（手动） | 构建工具自动编译 + 可选加密 |
| **客户端输入校验** | 运行时类型检查 | def 声明 + 自动生成校验代码 |
| **参数范围校验** | 无 | def 声明 minValue/maxValue/maxLength |
| **频率限制** | 无 | per-entity per-method 滑动窗口限流 |
| **Gateway 限流** | 无 | 连接级全局限流 |
| **异常捕获** | 部分覆盖 | 100% 覆盖，脚本 bug 不崩溃 |
| **超时保护** | 无 | ScriptGuard 自动超时检测 |
| **递归保护** | Python 默认 | 自定义限制 + 监控 |
| **热更验证** | 无 | 静态分析 + 试运行 + 审核流程 |
| **审计日志** | 无 | 结构化审计 + 监控 Metrics |
| **沙箱白名单** | 无 | **也不需要** |

---

## 7. 配置汇总

```xml
<!-- config/script-safety.xml -->
<scriptSafety>

    <!-- 源码保护 -->
    <deployment python="compiled" lua="compiled"/>

    <!-- 客户端防线 -->
    <clientGuard>
        <globalRateLimit maxPerSecond="100"/>
        <validation strict="true"/>
    </clientGuard>

    <!-- 运行时韧性 -->
    <resilience maxExecTimeMs="50"
                maxRecursionDepth="256"/>

    <!-- 热更验证 -->
    <hotupdate approvalRules="config/hotupdate-approval.xml"
               dryRunEnabled="true"
               auditLog="true"/>

</scriptSafety>
```
