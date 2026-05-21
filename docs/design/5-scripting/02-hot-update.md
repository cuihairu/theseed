# Hot Update — 热更新系统

> 热更新是游戏运维的核心能力，但也是最容易被过度承诺的能力。
> theseed 当前只把 `MVP` 能稳定兑现的部分写成实现基线。
>
> 来源：KBEngine Python reload()（有限支持），theseed 新增分级热更模型。
> 当前实现基线以 [../0-foundation/01-mvp-architecture-baseline](../0-foundation/01-mvp-architecture-baseline.md) 为准。

---

## 0. MVP 边界

```
MVP 只支持：
  - L1 配置热更
  - 受限的 L2 脚本实现替换

MVP 不支持：
  - L3 def 热更
  - L4 结构热更
  - 在线协议变更
  - 属性布局变更
  - 依赖脚本栈兼容的无感热替换
```

---

## 1. 热更新分级

```
L1: 配置热更（MVP 支持）
  ├── 游戏数值表、匹配规则、运营活动配置
  └── 无需任何代码变更

L2: 脚本热更（MVP 受限支持）
  ├── 修改现有方法实现
  ├── 修改定时器逻辑
  └── 不改方法签名 / 协议 / 属性布局

L3: 定义热更（Phase 2 以后考虑）
  ├── 添加新属性（有默认值）
  ├── 添加新 Entity 类型
  └── 需要自动 Schema 迁移和更强运行时兼容机制

L4: 结构变更（不做热更）
  ├── 删除属性
  ├── 修改属性类型
  ├── 修改网络协议
  └── 必须走滚动更新流程
```

---

## 2. L2 的严格约束

```
受限 L2 只允许：
  - 替换方法体
  - 调整局部业务流程
  - 调整定时器注册逻辑

受限 L2 明确禁止：
  - 修改方法签名
  - 修改 exposed 方法输入输出
  - 修改 Entity 属性定义
  - 修改序列化布局
  - 假设旧调用栈上的 frame 能与新代码无缝兼容
```

```
因此：
  “不重启进程” 不等于 “可以替换任何脚本语义”。
  只要变更影响协议、属性、持久化结构或迁移语义，
  一律升级为滚动发布，不走热更。
```

---

## 3. 热更新接口

```cpp
enum class HotUpdateLevel {
    L1_Config,
    L2_Script,
    L3_Def,
    L4_NeedRestart,
};

class IHotUpdateManager {
public:
    virtual HotUpdateLevel analyze(const DiffResult& diff) = 0;
    virtual HotUpdateResult apply(const HotUpdatePackage& pkg) = 0;
    virtual HotUpdateResult rollback(const std::string& version) = 0;
    virtual ValidationResult validate(const HotUpdatePackage& pkg) = 0;
};
```

```
MVP 实际实现要求：
  - analyze 必须能把超出 L2 边界的变更识别为 NeedRestart
  - apply 只对 L1 / 受限 L2 生效
  - rollback 必须优先保证可恢复，而不是追求无痕
```

---

## 4. 验证流程

```python
class HotUpdateValidator:
    def validate(self, old_defs, new_defs, old_scripts, new_scripts):
        # 1. Schema 兼容性检查
        # 2. exposed / 方法签名兼容性检查
        # 3. 脚本静态分析
        # 4. 试运行（隔离环境执行测试用例）
        return ValidationResult.merge(results)
```

### 4.1 MVP 必须阻断的变更

```
自动阻断：
  - change_property_type
  - remove_property
  - change_method_signature
  - change_exposed_protocol
  - change_entity_serialization

自动放行候选：
  - modify_script_body
  - modify_timer_logic
  - config_value_change
```

---

## 5. 审核配置

```xml
<hotUpdateApproval>
    <autoApprove>
        <rule changeType="config_value_change"/>
        <rule changeType="modify_script_body"/>
    </autoApprove>

    <manualReview>
        <rule changeType="modify_timer_logic"/>
    </manualReview>

    <forbidden>
        <rule changeType="remove_property"/>
        <rule changeType="change_property_type"/>
        <rule changeType="change_method_signature"/>
        <rule changeType="change_network_protocol"/>
    </forbidden>
</hotUpdateApproval>
```

---

## 6. 与迁移和异步的关系

```
热更新最容易与迁移、异步协程产生耦合风险。

MVP 的处理原则：
  - 不要求挂起协程在新旧代码间无缝续跑
  - 不要求迁移中的实体同时跨版本执行脚本栈
  - 迟到异步回调必须重新校验 entity epoch / version
```

```
也就是说：
  L2 热更的目标是“安全替换新入口逻辑”，
  不是“在线修改所有旧执行栈的语义”。
```

---

## 7. 运维自动化

### 7.1 滚动更新流程

```
Phase 1: Pre-check — 检查进程健康、版本兼容性
Phase 2: Drain — 标记 draining，停止新连接，等待实体迁移
Phase 3: Update — 拉取新二进制/脚本，重启进程
Phase 4: Verify — 健康检查、性能对比
Phase 5: Complete / Rollback
```

### 7.2 Control Plane

```cpp
class IControlPlane {
public:
    virtual ClusterStatus getClusterStatus() = 0;
    virtual DeployTask rollingUpdate(const std::string& version, ...) = 0;
    virtual HotUpdateTask hotUpdate(const HotUpdateConfig& config) = 0;
    virtual MigrationTask migrateEntities(ProcessId from, ProcessId to) = 0;
};
```

```
MVP 现实约束：
  - 热更新只是滚动发布的补充，不是替代
  - 只要变更越过 L2 边界，就必须走 rolling update
```

---

## 8. 与 KBEngine 的对比

| 能力 | KBEngine | theseed |
|------|---------|---------|
| 热更新 | Python reload() 有限 | MVP 支持 L1 + 受限 L2 |
| 部署 | 手动拷贝 + 重启 | 滚动更新 |
| 实体迁移 | 无自动迁移 | drain → migrate → restart |
| 配置管理 | XML 散落 | 配置中心 + 版本控制 |
