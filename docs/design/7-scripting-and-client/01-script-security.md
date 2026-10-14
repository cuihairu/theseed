# Script Security — Script-Layer Security and Stability

> **English** | [中文](01-script-security.zh.md)

> Core principle: **do not defend against insiders; defend against clients, bugs, and leaks**.
> Game server scripts are the developer's own business code and require no sandbox allowlist.
>
> Source assessment: neither BigWorld nor KBEngine has a complete script security governance layer.
> theseed adds script protection, stability, and exposure-surface governance.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
BigWorld's script focus is not a "security system" but rather:
  - Python integration
  - error output
  - watcher / reload assistance
  - runtime binding
```

### How KBEngine Implements It

```
KBEngine likewise has no built-in complete script security layer.
It leans more toward:
  - Python business scripts
  - runtime validation
  - a small amount of manual protection
```

### Pros and Cons

```
BigWorld / KBEngine pros:
  - simple and direct
  - do not add much complexity on the script side

BigWorld / KBEngine cons:
  - lack of source-code protection
  - lack of a unified argument defense line
  - lack of script stability governance
```

### theseed's Trade-offs

```
theseed chooses to extract script security into a separate layer,
because scripts are not only business implementations but also an external exposure surface and a source of incidents.

The cost is:
  - a more complex build chain
  - the need for unified def, rate, timeout, and exception protection
```

---

## 1. Source-Code Protection

### 1.1 Python: .pyc Deployment

```bash
# build process
theseed-build compile-scripts --source scripts/ --output deploy/scripts/ --strip-source
# deploy/scripts/ contains only .pyc bytecode, not .py source files
```

### 1.2 Lua: Precompiled Bytecode

```bash
theseed-build compile-scripts --source scripts/ --output deploy/scripts/
# generates .luac bytecode
```

### 1.3 Protection Levels

```
L1: .pyc / .luac precompiled deployment  → zero cost, prevents direct source copying
L2: encrypted packaging                  → AES encryption + in-memory decryption by the engine
L3: custom opcode (Lua)                  → modify the Lua VM opcode mapping
L4: push core logic down to C++          → scripts only do configuration and scheduling
```

---

## 2. Client Defense Line

### 2.1 Three-Layer Defense

```
client request → [Gateway rate limiting] → [def argument validation] → [script business validation]
```

### 2.2 def-Layer Declaration Defense

```xml
<Method name="attack" exposed="true" rateLimit="5/1s">
    <Arg name="targetId" type="ENTITY_ID" minValue="1"/>
    <Arg name="skillId" type="UINT32" minValue="1" maxValue="10000"/>
</Method>

<Method name="chat" exposed="true" rateLimit="3/1s">
    <Arg name="message" type="STRING" maxLength="200"/>
</Method>
```

### 2.3 Rate Limiting

```cpp
class MethodRateLimiter {
public:
    bool check(EntityId entityId, const std::string& method,
               int maxCalls, int windowSeconds);
    // sliding-window counter
};
```

---

## 3. Runtime Resilience

> This is not "security" but "stability". What it defends against is bugs, not attacks.

### 3.1 Timeout Protection

```cpp
class ScriptGuard {
    ScriptGuard(const std::string& context, Duration timeout);
    ~ScriptGuard();  // automatically records execution time, alerts on timeout
    bool checkTimeout() const;
};
```

### 3.2 Exception Catching

```cpp
void ScriptEngine::executeSafe(const std::string& context, std::function<void()> fn) {
    try {
        fn();
    } catch (const ScriptException& e) {
        LOG_ERROR("script error", {{"context", context}, {"error", e.what()}});
        METRIC_COUNTER("theseed.script.error", 1);
        // do not crash, keep running
    }
}
```

### 3.3 Recursion Depth Limit

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

Script security only cares about "don't blow up" and "don't leak";
for script debugging see `03-script-debug`, do not conflate the two.

---

## 4. Comparison with KBEngine

| Capability | KBEngine | theseed |
|------|---------|---------|
| Source-code protection | .pyc (manual) | build tool auto-compiles + optional encryption |
| Client input validation | runtime type checking | def declaration + auto-generated validation code |
| Argument range validation | none | minValue/maxValue/maxLength |
| Rate limiting | none | per-entity per-method sliding window |
| Exception catching | partial coverage | 100% coverage, script bugs do not crash |
| Timeout protection | none | ScriptGuard automatic timeout |
| Recursion protection | Python default | custom limit + monitoring |
