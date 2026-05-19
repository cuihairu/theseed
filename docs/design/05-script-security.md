# Script Security Design — 脚本层安全检查

> theseed 脚本层安全检查体系设计。
> 目标：虽然使用脚本（Python/Lua）做业务逻辑，但要提供多层安全防护，
> 防止脚本 bug 导致服务器崩溃、数据损坏、或被恶意利用。

---

## 1. 安全分层模型

```
┌─────────────────────────────────────────────────────┐
│ L1: 编译期检查 (def → 代码生成)                      │
│   类型安全、方法签名校验、属性声明校验                 │
├─────────────────────────────────────────────────────┤
│ L2: 加载期检查 (脚本加载时)                           │
│   沙箱隔离、API 白名单、资源限制                      │
├─────────────────────────────────────────────────────┤
│ L3: 运行时检查 (执行时)                               │
│   超时保护、内存限制、调用频率限制、异常捕获            │
├─────────────────────────────────────────────────────┤
│ L4: 运维期检查 (部署/运维时)                          │
│   热更验证、diff 审核、回滚保障                       │
└─────────────────────────────────────────────────────┘
```

---

## 2. L1: 编译期检查

### 2.1 Def 校验器 (theseed-defcheck)

```bash
# 在 CI 中自动运行
theseed-defcheck ./defs/

# 检查内容：
# 1. 类型一致性：属性类型在脚本中使用是否匹配 def 声明
# 2. 方法签名：脚本方法参数与 def 声明是否一致
# 3. 暴露安全：exposed 方法是否有合理的 rate_limit
# 4. 命名冲突：Entity 名称、属性名、方法名是否有冲突
# 5. 循环依赖：Entity 之间是否有循环引用
# 6. 存储兼容：属性变更是否兼容已有数据库 schema
```

### 2.2 检查规则

```yaml
# config/defcheck.yaml — 自定义检查规则

rules:
  # 类型安全
  - name: type_mismatch
    severity: error
    description: "脚本属性类型与 def 声明不匹配"
    example:
      # def 中声明 UINT32，脚本中赋值字符串 → error
      # level: UINT32 → self.level = "abc" → ERROR

  # 方法签名
  - name: method_signature_mismatch
    severity: error
    description: "脚本方法参数数量/类型与 def 声明不匹配"

  # 暴露方法安全
  - name: exposed_method_without_rate_limit
    severity: warning
    description: "exposed 方法没有声明 rate_limit"

  # 属性权限
  - name: client_writable_without_exposed
    severity: error
    description: "客户端可修改的属性必须通过 exposed 方法"

  # 存储安全
  - name: persistent_property_type_change
    severity: error
    description: "持久化属性类型变更需要提供迁移方案"

  # 大小限制
  - name: string_without_size
    severity: warning
    description: "STRING 类型建议声明 size 上限"
```

### 2.3 静态分析集成

```python
# Python 脚本：thised-defcheck 可以用 mypy 集成
# def 文件生成 .pyi 类型存根，mypy 自动检查

# 生成的类型存根示例 (Avatar.pyi)
from theseed import BaseEntity
from typing import Optional

class Avatar(BaseEntity):
    name: str
    level: int
    hp: float
    position: tuple[float, float, float]

    def attack(self, targetId: int, skillId: int) -> None: ...
    def onLevelUp(self, newLevel: int) -> None: ...
```

```lua
-- Lua 脚本：theseed-defcheck 生成 LuaLS (sumneko) 类型注解
-- 生成的类型定义文件

---@class Avatar: BaseEntity
---@field name string
---@field level integer
---@field hp number
---@field position Vector3
local Avatar = {}

---@param targetId integer
---@param skillId integer
function Avatar:attack(targetId, skillId) end

---@param newLevel integer
function Avatar:onLevelUp(newLevel) end
```

---

## 3. L2: 加载期检查

### 3.1 沙箱隔离

```python
# 脚本运行在受限环境中，只有白名单内的 API 可用

# 可用的 API（白名单）
SAFE_APIS = {
    # 引擎核心
    "theseed.getEntity",          # 获取实体
    "theseed.createEntity",       # 创建实体
    "theseed.destroyEntity",      # 销毁实体
    "theseed.callMethod",         # EntityCall

    # 计时器
    "theseed.addTimer",           # 添加定时器
    "theseed.cancelTimer",        # 取消定时器

    # 数据库
    "theseed.dbLoad",             # 数据库加载
    "theseed.dbSave",             # 数据库保存

    # Redis
    "theseed.redis.get",          # Redis 读取
    "theseed.redis.set",          # Redis 写入

    # 日志
    "theseed.log.info",           # 日志输出
    "theseed.log.warn",
    "theseed.log.error",

    # 数学
    "math.*",                     # 数学库

    # 集合
    "collections.*",              # 集合类型

    # 序列化
    "json.*",                     # JSON
}

# 禁止的 API（黑名单）
FORBIDDEN_APIS = {
    "os.system",                  # 不允许执行系统命令
    "os.exec*",                   # 不允许创建进程
    "subprocess.*",               # 不允许子进程
    "socket.*",                   # 不允许直接网络操作
    "open",                       # 不允许文件操作（通过引擎 API）
    "import os",                  # 不允许导入 os（受限版本除外）
    "eval",                       # 不允许动态执行
    "exec",                       # 不允许动态执行
    "__import__",                 # 不允许动态导入
    "sys.exit",                   # 不允许退出进程
    "ctypes.*",                   # 不允许 FFI
}
```

### 3.2 沙箱配置

```yaml
# config/script-sandbox.yaml

sandbox:
  # 模式
  mode: restricted               # restricted / permissive / disabled
                                  # restricted: 生产环境，严格白名单
                                  # permissive: 开发环境，仅禁止危险 API
                                  # disabled: 无限制（仅用于内部工具）

  # 白名单（restricted 模式下的可用模块）
  allowed_modules:
    - math
    - json
    - collections
    - theseed                     # 引擎 API
    - game                        # 游戏业务模块

  # 自定义禁止列表
  forbidden:
    - "os.system"
    - "subprocess"
    - "socket"

  # 资源限制
  limits:
    max_memory_per_entity: 1mb    # 单个实体最大内存
    max_cpu_time_per_tick: 50ms   # 单 tick 脚本最大 CPU 时间
    max_timer_per_entity: 100     # 单实体最大定时器数
    max_entity_per_space: 10000   # 单空间最大实体数
```

---

## 4. L3: 运行时检查

### 4.1 超时保护

```cpp
// script/ScriptGuard.h

class ScriptGuard {
public:
    // 构造时设置超时，析构时检查
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

        // 超时警告
        if (Clock::now() > deadline_) {
            LOG_WARN("script timeout", {
                {"context", context_},
                {"elapsed_ms", elapsedMs}
            });
            // 超时不直接杀线程（不安全），而是标记并告警
            // 严重的可以在下一个 tick 采取行动
        }
    }

    // 检查是否超时（脚本内定期调用，如循环中）
    bool checkTimeout() const {
        return Clock::now() > deadline_;
    }

private:
    std::string context_;
    TimePoint deadline_;
    TimePoint startTime_;
};

// 使用方式
void ScriptEngine::executeMethod(EntityId id, const std::string& method) {
    ScriptGuard guard(fmt::format("entity.{}.{}", id, method),
                      config_.maxScriptExecTime);

    // 执行脚本方法
    // 脚本内可以检查 guard.checkTimeout() 来主动退出长循环
    doExecute(id, method);
}
```

### 4.2 内存限制

```python
# 脚本层内存限制

class MemoryLimiter:
    """
    跟踪每个实体的脚本对象内存使用
    超过限制时阻止新分配并告警
    """

    def __init__(self, entity_id: int, max_bytes: int):
        self.entity_id = entity_id
        self.max_bytes = max_bytes
        self.current_bytes = 0

    def on_alloc(self, size: int):
        self.current_bytes += size
        if self.current_bytes > self.max_bytes:
            theseed.log.error(
                f"Entity {self.entity_id} memory limit exceeded: "
                f"{self.current_bytes} > {self.max_bytes}"
            )
            # 选项 1: 阻止分配（抛异常）
            # 选项 2: 触发 GC
            # 选项 3: 标记实体为异常状态
```

### 4.3 异常捕获

```cpp
// 所有的脚本调用都被 try-catch 包裹
void ScriptEngine::executeSafe(const std::string& context,
                                std::function<void()> fn) {
    try {
        fn();
    } catch (const ScriptException& e) {
        // 脚本异常（Python Exception / Lua Error）
        LOG_ERROR("script error", {
            {"context", context},
            {"error", e.what()},
            {"type", e.typeName()},
            {"traceback", e.traceback()}
        });

        METRIC_COUNTER("theseed.script.error_count", 1, {
            {"context", context},
            {"type", e.typeName()}
        });

        // 选项：
        // 1. 继续运行（默认，游戏服务器不能因为一个脚本 bug 崩溃）
        // 2. 标记实体为 error 状态
        // 3. 通知客户端显示错误

    } catch (const std::exception& e) {
        // C++ 异常（严重）
        LOG_FATAL("unexpected error in script context", {
            {"context", context},
            {"error", e.what()}
        });
        // 这种情况可能需要重启进程
    }
}
```

### 4.4 调用频率限制

```python
# Exposed 方法的频率限制（防止客户端恶意刷接口）

class RateLimiter:
    """每个 exposed 方法的调用频率限制"""

    def __init__(self):
        self._counters = {}  # entity_id → method → Counter

    def check(self, entity_id: int, method: str, limit: RateLimit) -> bool:
        key = (entity_id, method)
        counter = self._counters.get(key)

        if counter is None:
            counter = SlidingWindowCounter(limit.window_seconds)
            self._counters[key] = counter

        counter.increment()

        if counter.count() > limit.max_calls:
            theseed.log.warn(
                f"rate limit exceeded: entity={entity_id} method={method} "
                f"count={counter.count()} limit={limit.max_calls}"
            )
            return False   # 拒绝

        return True        # 允许


# def 中声明限制
# methods:
#   attack:
#     exposed: true
#     rate_limit: { max: 5, window: 1 }  # 每秒最多 5 次
```

### 4.5 调用栈深度限制

```python
# 防止脚本无限递归

MAX_RECURSION_DEPTH = 256  # 默认最大递归深度

# Python: sys.setrecursionlimit() 在沙箱中设为受限值
# Lua: debug.sethook() 监控调用栈深度

def check_recursion_depth():
    current = get_current_depth()
    if current > MAX_RECURSION_DEPTH:
        raise RecursionError(
            f"Script recursion depth exceeded: {current} > {MAX_RECURSION_DEPTH}"
        )
```

---

## 5. L4: 运维期检查

### 5.1 热更验证

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

                # 类型变更
                if old_prop.type != new_prop.type:
                    errors.append(
                        f"INCOMPATIBLE: {entity_name}.{prop_name} type changed "
                        f"from {old_prop.type} to {new_prop.type}"
                    )

                # 删除持久化属性
                if old_prop.persistent and not new_prop.persistent:
                    errors.append(
                        f"WARNING: {entity_name}.{prop_name} removed from persistent"
                    )

        return ValidationResult(errors)
```

### 5.2 变更审核

```yaml
# 热更审核流程

hot_update_approval:
  # 自动通过的条件
  auto_approve:
    - change_type: add_property
      has_default: true
    - change_type: add_method
    - change_type: modify_script_body   # 只改脚本实现，不改接口

  # 需要人工审核的变更
  manual_review:
    - change_type: remove_property
    - change_type: change_property_type
    - change_type: modify_exposed_method_signature
    - change_type: add_exposed_method

  # 禁止的变更（必须走滚动重启）
  forbidden:
    - change_type: remove_entity_type
    - change_type: change_network_protocol
```

---

## 6. 安全事件审计

### 6.1 审计日志

```json
{
  "event_type": "script_violation",
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

### 6.2 安全指标

```
# 安全相关的 Metrics

theseed_security_violation_total    # 安全违规计数 (Counter)
  - labels: type (rate_limit, sandbox, memory, timeout, recursion)
  - labels: entity_type

theseed_script_error_total          # 脚本错误计数 (Counter)
  - labels: type (runtime, type_error, attribute_error, ...)
  - labels: entity_type

theseed_sandbox_block_total         # 沙箱拦截计数 (Counter)
  - labels: api (os.system, socket, file, ...)

theseed_hotupdate_validation_result # 热更验证结果 (Counter)
  - labels: result (pass, warn, fail)
```

---

## 7. 与 KBEngine 的对比

| 安全能力 | KBEngine | theseed |
|---------|---------|---------|
| **类型检查** | 运行时 | 编译期（defcheck + mypy/LuaLS） |
| **沙箱** | 无 | 白名单 + 黑名单双模式 |
| **超时保护** | 无 | ScriptGuard 自动超时 |
| **内存限制** | 无 | 按实体限制 + 告警 |
| **异常捕获** | Python try-catch 不完全 | 100% 覆盖 + 不崩溃 |
| **频率限制** | 无 | per-entity per-method 限流 |
| **递归保护** | Python 默认 | 自定义限制 + 监控 |
| **热更验证** | 无 | 静态分析 + 试运行 + 审核 |
| **审计日志** | 无 | 结构化审计 + 安全 Metrics |
| **Exposed 安全** | 无验证 | 类型 + 范围 + 频率 + 存在性 |

---

## 8. 配置汇总

```yaml
# config/script-security.yaml

script_security:
  # L1: 编译期
  defcheck:
    enabled: true
    rules: config/defcheck.yaml
    mypy_strict: true                   # Python mypy 严格模式
    luals_strict: true                  # Lua LuaLS 严格模式

  # L2: 加载期
  sandbox:
    mode: restricted                    # restricted / permissive / disabled
    allowed_modules: [math, json, collections, theseed, game]
    forbidden_apis: [os.system, subprocess, socket, eval, exec]

  # L3: 运行时
  runtime:
    max_exec_time_ms: 50               # 单次脚本执行超时
    max_memory_per_entity_kb: 1024     # 单实体内存限制
    max_timers_per_entity: 100         # 单实体定时器限制
    max_recursion_depth: 256           # 递归深度限制
    default_rate_limit:                # 默认频率限制
      max: 10
      window: 1                        # 每秒 10 次

  # L4: 运维期
  hotupdate:
    auto_approve_rules: config/hotupdate-approval.yaml
    dry_run_enabled: true
    audit_log: true
```
