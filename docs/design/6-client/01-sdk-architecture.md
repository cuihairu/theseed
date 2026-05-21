# Client SDK — 客户端引擎支持

> theseed 客户端 SDK：一份 XML 定义 → 自动生成 Unity/UE5/Cocos 客户端代码。
> 开发者不需要手写同步逻辑。
>
> 来源：KBEngine 有 Unity 插件（维护不活跃），theseed 新增三大引擎全覆盖 + 代码生成。

---

## 1. 架构总览

```
Def 文件 (XML + XSD)
  "一份定义，四处生成"
    ├── Server C++ Entity 类
    ├── Client C# (Unity) 类
    ├── Client C++ (UE5) 类
    └── Client TypeScript (Cocos) 类

Runtime SDK (每个引擎的运行时库)
  ├── Network Layer (TCP / WebSocket)
  ├── Entity Sync Engine (属性同步、插值、预测)
  ├── Event System (消息收发)
  └── Debug Tools (编辑器内嵌)
```

---

## 2. 代码生成

```bash
theseed-codegen \
  --defs ./defs/ \
  --output-unity ./client/unity/Assets/Theseed/Generated/ \
  --output-ue5 ./client/ue5/Plugins/Theseed/Source/Generated/ \
  --output-cocos ./client/cocos/assets/theseed/generated/ \
  --output-server ./src/generated/
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

### UE5 / C++ 示例

```cpp
UCLASS(BlueprintType, Blueprintable)
class THESEED_API AAvatar : public ATheseedEntity {
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintReadOnly) FString name;
    UPROPERTY(BlueprintReadOnly) int32 level;
    UPROPERTY(BlueprintReadOnly) float hp;

    UFUNCTION(BlueprintImplementableEvent) void OnLevelUp(int32 newLevel);
    UFUNCTION(BlueprintCallable) void Attack(int64 targetId, int32 skillId);
};
```

### Cocos / TypeScript 示例

```typescript
export class Avatar extends EntityBase {
    readonly hp = new InterpolatedProperty<number>(5.0);
    readonly position = new InterpolatedProperty<Vec3>(10.0);

    protected onLevelUp(newLevel: number): void {}
    public attack(targetId: number, skillId: number): void {
        this.callServer('attack', targetId, skillId);
    }
}
```

---

## 3. Runtime SDK

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
    public T current { get; }   // 当前插值后的值（渲染用）
    public T target { get; }    // 服务器最新值
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

## 6. 与 KBEngine 的对比

| 能力 | KBEngine | theseed |
|------|---------|---------|
| Unity | 有插件（维护不活跃） | 自动生成 + 编辑器集成 |
| UE5 | 无 | Blueprint 全集成 |
| Cocos | 无 | TypeScript 自动生成 |
| 协议生成 | 无（手写客户端） | theseed-codegen 自动生成 |
| 属性插值 | 无 | InterpolatedProperty |
| 断线重连 | 有限 | 自动重连 + 服务器保持 |
| 编辑器工具 | 无 | Entity Viewer + Profiler |
