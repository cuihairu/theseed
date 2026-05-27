# Client SDK — 客户端引擎支持

> theseed 客户端 SDK：一份 XML 定义 → 生成服务端与客户端协议代码。
> 同时保留一条“SDK 运行时元数据导入/更新”链路，用来参考 KBEngine 的混合式客户端方案。
> 但 `MVP` 不追求三端一次到位，而是先保证 Unity 全链路跑通。
>
> 来源源头：BigWorld 的客户端协议与实体同步思想。
> 参考实现：KBEngine 的 Unity 插件与 SDK 生成链（维护不活跃）。
> theseed 新增多引擎代码生成目标。
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
KBEngine 不是纯“运行时动态、完全不生成代码”：
  - Unity / UE4 走离线 SDK 生成链
  - 运行时再导入 entitydef / messages 摘要
  - 通过 EntityDef.moduledefs + 反射把脚本类绑定起来
  - JS 路径还有运行时 moduledefs 导入链

它的特点是“生成 + 运行时绑定”混合在一起，
而不是 theseed 想要的“编译期主生成、运行期只做受控导入”分层。
```

### 优缺点

```
BigWorld / KBEngine 的优点：
  - 协议语义成熟
  - 实体同步路径清楚

BigWorld / KBEngine 的缺点：
  - KBEngine 的生成链和运行时绑定耦合偏强
  - 多端统一能力不够
  - 现代化 SDK 分发/更新边界不够清晰
```

### theseed 的取舍

```
theseed 不追求一开始就搬运 BigWorld 的完整客户端生态，
也不接受把 KBEngine 的手写客户端协议路径作为长期主线。

取舍是：
  - 先保证 Unity 主路径
  - 把 UE5 / Cocos 放到后续阶段
  - 用 codegen 换取一致性
  - 同时预留 SDK 元数据导入/更新链，作为后续兼容与工具链能力
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

SDK Delivery / Import
  ├── 生成产物分发
  ├── 元数据摘要校验
  └── 受控导入/替换（Phase 2 预留）
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

### 2.3 运行时 SDK 更新预留

```
参考 KBEngine 的混合式客户端方案，theseed 预留客户端 SDK 分发与导入链：
  - 服务端下发 def / digest / package 摘要
  - 客户端校验版本与兼容性
  - 按受控流程替换生成产物

注意：
  这不是 MVP 主路径；
  MVP 仍然以离线 codegen + 正常发布为主。
```

### 2.4 用于差异定位的摘要树

```
如果只用单个 MD5 / digest，
它只能回答：
  - “整体是否发生变化”

却不能快速回答：
  - “是哪个 entity / message / property 变了”
  - “需要重建哪一部分缓存”
  - “客户端是否可以只增量拉取一部分元数据”
```

```
因此 theseed 可以把摘要结构升级为 Merkle tree：
  - leaf：entity / property / method / message / schema node
  - branch：聚合后的局部摘要
  - root：对外发布的整体版本摘要
```

```
收益：
  - 快速定位变更子树
  - 便于增量同步与局部重新校验
  - 比单个 digest 更适合大协议面
```

### 2.5 兼容性不由 hash 单独决定

```
Merkle tree 只能回答“哪里变了”，
不能单独回答“是否兼容”。
```

兼容性仍然要靠语义规则：

```
可兼容候选：
  - 新增字段，且有默认值
  - 新增非必需方法
  - 新增可选消息

通常不兼容：
  - 删除字段
  - 修改字段类型
  - 修改协议编号 / method id / alias id
  - 修改序列化顺序
  - 修改客户端必须理解的枚举语义
```

### 2.6 推荐判定流程

```
1. 先做 canonicalization
   - 避免仅仅重排文本就改变摘要

2. 再做 Merkle diff
   - 快速定位变更子树

3. 最后做 semantic compatibility check
   - 判定 backward / forward / incompatible
```

### 2.7 theseed 的取舍

```
theseed 不应继续停留在 KBEngine / BigWorld 风格的“单个 digest 比较”。

更合理的边界是：
  - 对外仍可暴露 root digest 作为快速握手摘要
  - 内部使用 Merkle tree 做 diff、缓存、增量分发
  - 最终兼容性结论由 schema / protocol 规则引擎给出
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
| Unity | 有插件 + 离线生成 SDK + 运行时导入 | MVP 主支持 |
| UE5 | 无 | Phase 2 目标 |
| Cocos | 无 | Phase 2 目标 |
| 协议生成 | 部分存在（客户端 SDK 生成链） | theseed-codegen |
| 属性插值 | 无 | InterpolatedProperty |
| 断线重连 | 有限 | 自动重连 + 服务器保持 |
| SDK 更新链 | 有限但存在 | Phase 2 预留 |
