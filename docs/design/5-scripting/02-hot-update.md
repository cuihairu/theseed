# Hot Update — 热更新系统

> 热更新是游戏运维的核心能力。theseed 提供分级热更 + 安全验证 + 一键回滚。
>
> 来源：KBEngine Python reload()（有限支持），theseed 新增完整热更体系。

---

## 1. 热更新分级

```
L1: 配置热更（最安全）
  ├── 游戏数值表、匹配规则、运营活动配置
  └── 无需任何代码变更

L2: 脚本热更（中等风险）
  ├── 修改方法实现、添加新方法、修改定时器逻辑
  └── 不重启进程、不丢失玩家状态

L3: 定义热更（高风险）
  ├── 添加新属性（有默认值）、添加新 Entity 类型
  └── 需要自动 Schema 迁移

L4: 结构变更（不可热更，需滚动重启）
  ├── 删除属性、修改属性类型、修改网络协议
  └── 必须走滚动更新流程
```

---

## 2. 热更新接口

```cpp
enum class HotUpdateLevel { L1_Config, L2_Script, L3_Def, L4_NeedRestart };

class IHotUpdateManager {
public:
    virtual HotUpdateLevel analyze(const DiffResult& diff) = 0;
    virtual HotUpdateResult apply(const HotUpdatePackage& pkg) = 0;
    virtual HotUpdateResult rollback(const std::string& version) = 0;
    virtual ValidationResult validate(const HotUpdatePackage& pkg) = 0;
};
```

---

## 3. 验证流程

```python
class HotUpdateValidator:
    def validate(self, old_defs, new_defs, old_scripts, new_scripts):
        # 1. Schema 兼容性检查
        # 2. 接口兼容性检查（方法签名变更）
        # 3. 脚本静态分析
        # 4. 试运行（隔离环境执行新脚本的测试用例）
        return ValidationResult.merge(results)
```

---

## 4. 审核配置

```xml
<hotUpdateApproval>
    <autoApprove>
        <rule changeType="add_property" hasDefault="true"/>
        <rule changeType="modify_script_body"/>
    </autoApprove>

    <manualReview>
        <rule changeType="remove_property"/>
        <rule changeType="change_property_type"/>
    </manualReview>

    <forbidden>
        <rule changeType="remove_entity_type"/>
        <rule changeType="change_network_protocol"/>
    </forbidden>
</hotUpdateApproval>
```

---

## 5. 运维自动化

### 5.1 滚动更新流程

```
Phase 1: Pre-check — 检查进程健康、配置兼容性
Phase 2: Drain — 标记 draining，停止新连接，等待实体迁移
Phase 3: Update — 拉取新二进制/脚本，重启进程
Phase 4: Verify — 全集群健康检查、性能对比
Phase 5: Complete / Rollback
```

### 5.2 Control Plane

```cpp
class IControlPlane {
public:
    virtual ClusterStatus getClusterStatus() = 0;
    virtual DeployTask rollingUpdate(const std::string& version, ...) = 0;
    virtual HotUpdateTask hotUpdate(const HotUpdateConfig& config) = 0;
    virtual MigrationTask migrateEntities(ProcessId from, ProcessId to) = 0;
};
```

---

## 6. 与 KBEngine 的对比

| 能力 | KBEngine | theseed |
|------|---------|---------|
| 热更新 | Python reload() 有限 | L1-L3 分级热更 + 验证 + 回滚 |
| 部署 | 手动拷贝 + 重启 | 一键滚动更新 |
| 实体迁移 | 无自动迁移 | drain → migrate → restart |
| 配置管理 | XML 散落 | 配置中心 + 版本控制 |
