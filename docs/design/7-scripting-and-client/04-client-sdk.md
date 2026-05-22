# Client SDK — 客户端引擎支持

> theseed 客户端 SDK：一份 XML 定义 → 生成服务端与客户端协议代码。
> 但 `MVP` 不追求三端一次到位，而是先保证 Unity 全链路跑通。
>
> 来源：KBEngine 有 Unity 插件（维护不活跃），theseed 新增多引擎代码生成目标。
> 当前实现基线以 [../0-foundation/01-mvp-architecture-baseline](../0-foundation/01-mvp-architecture-baseline.md) 为准。

---

## 0.5 引擎实现对照与取舍

### BigWorld 是怎么实现的

```
BigWorld 有成熟客户端协议和完整生态，
但这次审计口径不把客户端渲染与编辑器生态计入必做项。

因此这篇只借鉴它的协议与实体同步思想，
不照搬整套客户端体系。
```

### KBEngine 是怎么实现的

```
KBEngine 提供过 Unity 插件与客户端接入样板，
但整体更偏：
  - 手写客户端逻辑
  - 协议生成较弱
  - 多端能力不成体系
```

### 优缺点

```
BigWorld / KBEngine 的优点：
  - 协议语义成熟
  - 实体同步路径清楚

BigWorld / KBEngine 的缺点：
  - 现代 codegen 弱
  - 多端统一差
  - 客户端工程化能力不够统一
```

### theseed 的取舍

```
theseed 不追求一开始就复制 BigWorld 的完整客户端生态，
也不接受 KBEngine 式长期手写客户端协议。

取舍是：
  - 先保证 Unity 主路径
  - 把 UE5 / Cocos 放到后续阶段
  - 用 codegen 换取一致性
```

---

## 0. MVP 边界

```
MVP 客户端只保证 Unity 端全链路可用：
  - 登录
  - 断线重连
  - 实体创建 / 销毁
  - 属性同步
  - 位置插值
  - exposed 方法调用

UE5 / Cocos 保留为 Phase 2 目标，不作为 MVP 完成条件。
```

---

## 1. 架构总览

```
Def 文件 (XML + XSD)
  "一份定义，多处生成"
    ├── Server C++ Entity 类
    ├── Client C# (Unity) 类      ← MVP 主路径
    ├── Client C++ (UE5) 类       ← Phase 2
    └── Client TypeScript (Cocos) 类 ← Phase 2

Runtime SDK
  ├── Network Layer
  ├── Entity Sync Engine
  ├── Event System
  └── Runtime Inspect Helpers
```

---

## 2. 代码生成

### 2.1 MVP 输出

```bash
theseed-codegen \
  --defs ./defs/ \
  --output-unity ./client/unity/Assets/Theseed/Generated/ \
  --output-server ./src/generated/
```

### 2.2 Phase 2 预留

```bash
theseed-codegen \
  --defs ./defs/ \
  --output-ue5 ./client/ue5/Plugins/Theseed/Source/Generated/ \
  --output-cocos ./client/cocos/assets/theseed/generated/
```

```
说明：
  - “支持 UE5 / Cocos” 现在是产品方向，不是 MVP 交付定义
  - 不应让多端生成阻塞 Runtime Core 和 Unity 主链路
```

### Unity / C# 示例

```csharp
public partial class Avatar : EntityBase {
    public SyncProperty<string> name { get; }
    public SyncProperty<uint> level { get; }
    public InterpolatedProperty<float> hp { get; }
    public InterpolatedProperty<Vector3> position { get; }

    partial void OnLevelUp(uint newLevel);
    partial void OnDamaged(float damage, ulong attackerId);

    public void attack(ulong targetId, uint skillId) => CallServer("attack", targetId, skillId);
}
```

---

## 3. Unity Runtime SDK

### 网络层

```csharp
public class NetworkClient {
    public async Task Connect(string host, int port);
    public async Task ConnectWs(string url);
    public async Task<AuthResult> Login(string account, string token);
    public void EnableAutoReconnect(ReconnectConfig config);
}
```

### 实体同步引擎

```csharp
public class EntitySyncEngine {
    public event Action<EntityBase> OnEntityEnter;
    public event Action<EntityBase> OnEntityLeave;
    public EntityBase PlayerEntity { get; }
    public EntityBase GetEntity(ulong entityId);
}

public class InterpolatedProperty<T> {
    public T current { get; }
    public T target { get; }
    public void SetTarget(T value);
    public void Update(float dt);
}
```

### 插值策略

```
属性类型          插值策略
position         线性插值 + 客户端预测
rotation         球面插值 (SLERP)
hp/mp            线性插值
level/name       不插值（直接跳变）
```

---

## 4. Exposed 方法安全

```
信任边界：
  - 只有 exposed=true 的方法才会生成客户端 CallServer 函数
  - 服务器端自动校验：类型检查 + 范围检查 + 频率限制
  - 非 exposed 方法不出现在客户端生成代码中
```

---

## 5. 带宽优化

```
1. detail_level 分级同步
2. alias 机制（1 字节代替 entity type string）
3. 属性 delta（只发送变化的属性）
4. 消息合并（tick 内多个变更合并为一条消息）
```

---

## 6. Phase 2 方向

```
UE5 方向：
  - 代码生成
  - Blueprint 可调用 API
  - 与 Unity 对齐的实体同步语义

Cocos 方向：
  - TypeScript 代码生成
  - 轻量运行时 SDK
  - WebSocket 优先接入
```

```
但在 Phase 2 之前，不承诺：
  - UE5 Blueprint 深度集成
  - 三端编辑器工具等价
  - 三端网络栈完全同构
```

---

## 7. 与 KBEngine 的对比

| 能力 | KBEngine | theseed |
|------|---------|---------|
| Unity | 有插件（维护不活跃） | MVP 主支持 |
| UE5 | 无 | Phase 2 目标 |
| Cocos | 无 | Phase 2 目标 |
| 协议生成 | 无（手写客户端） | theseed-codegen |
| 属性插值 | 无 | InterpolatedProperty |
| 断线重连 | 有限 | 自动重连 + 服务器保持 |
| 编辑器工具 | 无 | Unity 主路径优先 |
