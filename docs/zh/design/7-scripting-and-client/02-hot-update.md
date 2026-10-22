# Hot Update — Hot-Update System

> English | **中文**

> Hot update is a core capability of game operations, but it is also the capability most easily over-promised.
> theseed currently writes only the parts that `MVP` can reliably deliver into the implementation baseline.
>
> Origin: BigWorld's controlled operations and rolling-replacement boundary.
> Reference implementation: KBEngine's limited script-replacement path via Python reload().
> theseed adds a tiered hot-update model.
> The current implementation baseline follows [../0-foundation/01-mvp-architecture-baseline](../0-foundation/01-mvp-architecture-baseline.md).

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
BigWorld has no "arbitrary script hot-replacement platform" in the modern sense.

It relies more on:
  - operations control
  - process lifecycle management
  - data and HA toolchains
  - cautious rolling replacement
```

### How KBEngine Implements It

```
KBEngine provides a limited Python reload() path.

Its characteristics are:
  - can do limited script replacement
  - not good at structural-level changes
  - no complete version-consistency governance
```

### Pros and Cons

```
Pros of BigWorld's origin boundary:
  - stable
  - more conservative about consistency boundaries

Cons of BigWorld's origin boundary:
  - small online changes are costly

Pros of KBEngine's reference implementation:
  - lightweight
  - fast to change scripts

Cons of KBEngine's reference implementation:
  - easy to overestimate hot-update capability
  - version, migration, and async boundaries are not hard enough
```

### theseed's Trade-offs

```
theseed takes the middle ground between the two:
  - keep the script hot-update convenience of the KBEngine reference implementation
  - use the BigWorld origin-boundary constraints to limit capability claims

Therefore:
  - MVP supports only L1 + restricted L2
  - structural-level changes always fall back to rolling update
```

---

## 0. MVP Boundary

```
MVP supports only:
  - L1 config hot update
  - restricted L2 script-implementation replacement

MVP does not support:
  - L3 def hot update
  - L4 structural hot update
  - online protocol changes
  - property-layout changes
  - seamless hot replacement that depends on script-stack compatibility
```

---

## 1. Hot-Update Tiers

```
L1: config hot update (MVP-supported)
  ├── game numeric tables, matchmaking rules, live-ops activity config
  └── no code changes required

L2: script hot update (MVP restricted support)
  ├── modify existing method implementations
  ├── modify timer logic
  └── do not change method signatures / protocols / property layout

L3: definition hot update (considered after Phase 2)
  ├── add new properties (with default values)
  ├── add new Entity types
  └── requires automatic Schema migration and a stronger runtime-compatibility mechanism

L4: structural change (no hot update)
  ├── remove properties
  ├── change property types
  ├── change network protocols
  └── must go through the rolling-update process
```

---

## 2. Strict Constraints on L2

```
Restricted L2 allows only:
  - replacing method bodies
  - adjusting local business flow
  - adjusting timer registration logic

Restricted L2 explicitly forbids:
  - modifying method signatures
  - modifying the inputs/outputs of exposed methods
  - modifying Entity property definitions
  - modifying the serialization layout
  - assuming that frames on the old call stack can seamlessly interoperate with new code
```

```
Therefore:
  "not restarting the process" does not equal "being able to replace any script semantics".
  As long as a change affects the protocol, properties, persistence structure, or migration semantics,
  it is escalated to a rolling release and does not go through hot update.
```

---

## 3. Hot-Update Interface

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
MVP actual implementation requirements:
  - analyze must be able to identify changes beyond the L2 boundary as NeedRestart
  - apply takes effect only for L1 / restricted L2
  - rollback must prioritize recoverability over a seamless, traceless result
```

---

## 4. Validation Flow

```python
class HotUpdateValidator:
    def validate(self, old_defs, new_defs, old_scripts, new_scripts):
        # 1. Schema compatibility check
        # 2. exposed / method-signature compatibility check
        # 3. script static analysis
        # 4. trial run (execute test cases in an isolated environment)
        return ValidationResult.merge(results)
```

### 4.1 Changes MVP Must Block

```
Automatically blocked:
  - change_property_type
  - remove_property
  - change_method_signature
  - change_exposed_protocol
  - change_entity_serialization

Automatic-pass candidates:
  - modify_script_body
  - modify_timer_logic
  - config_value_change
```

---

## 5. Approval Configuration

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

## 6. Relationship with Migration and Async

```
Hot update is most prone to coupling risk with migration and async coroutines.

MVP's handling principles:
  - do not require suspended coroutines to seamlessly resume across old and new code
  - do not require entities under migration to simultaneously execute script stacks across versions
  - late async callbacks must re-validate entity epoch / version
```

```
In other words:
  the goal of L2 hot update is to "safely replace new entry-point logic",
  not to "modify the semantics of all old execution stacks online".
```

Script debugging needs to treat `entityEpoch / scriptVersion / suspensionId` as the session boundary,
see `03-script-debug`.

---

## 7. Operations Automation

### 7.1 Rolling-Update Process

```
Phase 1: Pre-check — check process health and version compatibility
Phase 2: Drain — mark draining, stop new connections, wait for entity migration
Phase 3: Update — pull the new binary/scripts, restart the process
Phase 4: Verify — health check, performance comparison
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
MVP practical constraints:
  - hot update is only a supplement to rolling release, not a replacement
  - as soon as a change crosses the L2 boundary, it must go through rolling update
```

---

## 8. Comparison with KBEngine

| Capability | KBEngine | theseed |
|------|---------|---------|
| Hot update | Python reload() limited | MVP supports L1 + restricted L2 |
| Deployment | manual copy + restart | rolling update |
| Entity migration | no automatic migration | drain → migrate → restart |
| Config management | scattered XML | config center + version control |
