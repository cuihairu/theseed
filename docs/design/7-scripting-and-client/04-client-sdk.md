# Client SDK — Client Engine Support

> **English** | [中文](04-client-sdk.zh.md)

> theseed client SDK: one XML definition → generate server and client protocol code.
> It also retains an "SDK runtime metadata import/update" chain, to reference KBEngine's hybrid client approach.
> But `MVP` does not aim to deliver all three platforms at once; it first ensures the full Unity chain works end to end.
>
> Origin: BigWorld's client protocol and entity-synchronization ideas.
> Reference implementation: KBEngine's Unity plugin and SDK generation chain (maintenance inactive).
> theseed adds multi-engine code-generation targets.
> The current implementation baseline follows [../0-foundation/01-mvp-architecture-baseline](../0-foundation/01-mvp-architecture-baseline.md).

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
BigWorld has a mature client protocol and a complete ecosystem,
but this audit's definition does not count client rendering and the editor ecosystem as must-do items.

Therefore this document borrows only its protocol and entity-synchronization ideas,
and does not copy the entire client system wholesale.
```

### How KBEngine Implements It

```
KBEngine is not purely "runtime dynamic, no code generation at all":
  - Unity / UE4 go through an offline SDK generation chain
  - the runtime then imports entitydef / messages summaries
  - script classes are bound via EntityDef.moduledefs + reflection
  - the JS path also has a runtime moduledefs import chain

Its characteristic is that "generation + runtime binding" are mixed together,
rather than the "compile-time primary generation, runtime controlled import only" layering that theseed wants.
```

### Pros and Cons

```
Pros of BigWorld / KBEngine:
  - mature protocol semantics
  - clear entity-synchronization path

Cons of BigWorld / KBEngine:
  - KBEngine's generation chain and runtime binding are somewhat strongly coupled
  - insufficient cross-platform unification
  - the boundary for modern SDK distribution/update is not clear enough
```

### theseed's Trade-offs

```
theseed does not aim to port BigWorld's complete client ecosystem from the start,
nor does it accept KBEngine's hand-written client protocol path as the long-term mainline.

The trade-off is:
  - ensure the Unity main path first
  - place UE5 / Cocos in later phases
  - trade codegen for consistency
  - meanwhile reserve the SDK metadata import/update chain as a later compatibility and toolchain capability
```

---

## 0. MVP Boundary

```
The MVP client guarantees only that the full Unity-side chain is usable:
  - login
  - reconnect after disconnection
  - entity creation / destruction
  - property synchronization
  - position interpolation
  - exposed method invocation

UE5 / Cocos remain Phase 2 targets and are not MVP completion criteria.
```

---

## 1. Architecture Overview

```
Def files (XML + XSD)
  "one definition, generated in multiple places"
    ├── Server C++ Entity class
    ├── Client C# (Unity) class      ← MVP main path
    ├── Client C++ (UE5) class       ← Phase 2
    └── Client TypeScript (Cocos) class ← Phase 2

Runtime SDK
  ├── Network Layer
  ├── Entity Sync Engine
  ├── Event System
  └── Runtime Inspect Helpers

SDK Delivery / Import
  ├── generated artifact distribution
  ├── metadata digest validation
  └── controlled import/replacement (reserved for Phase 2)
```

---

## 2. Code Generation

### 2.1 MVP Output

```bash
theseed-codegen \
  --defs ./defs/ \
  --output-unity ./client/unity/Assets/Theseed/Generated/ \
  --output-server ./src/generated/
```

### 2.2 Phase 2 Reserve

```bash
theseed-codegen \
  --defs ./defs/ \
  --output-ue5 ./client/ue5/Plugins/Theseed/Source/Generated/ \
  --output-cocos ./client/cocos/assets/theseed/generated/
```

### 2.3 Runtime SDK Update Reserve

```
Referencing KBEngine's hybrid client approach, theseed reserves a client SDK distribution and import chain:
  - the server distributes def / digest / package summaries
  - the client validates version and compatibility
  - generated artifacts are replaced via a controlled process

Note:
  this is not the MVP main path;
  MVP still centers on offline codegen + normal release.
```

### 2.4 Digest Tree for Diff Localization

```
If only a single MD5 / digest is used,
it can answer only:
  - "whether the whole has changed"

but it cannot quickly answer:
  - "which entity / message / property changed"
  - "which part of the cache needs to be rebuilt"
  - "whether the client can incrementally pull only part of the metadata"
```

```
Therefore theseed can upgrade the digest structure into a Merkle tree:
  - leaf: entity / property / method / message / schema node
  - branch: aggregated local digest
  - root: the overall version digest published externally
```

```
Benefits:
  - quickly locate the changed subtree
  - facilitate incremental synchronization and local re-validation
  - better suited to a large protocol surface than a single digest
```

### 2.5 Compatibility Is Not Determined by Hash Alone

```
A Merkle tree can only answer "what changed";
it cannot answer "whether it is compatible" on its own.
```

Compatibility still relies on semantic rules:

```
Compatible candidates:
  - adding a field, with a default value
  - adding a non-required method
  - adding an optional message

Usually incompatible:
  - deleting a field
  - changing a field type
  - changing a protocol number / method id / alias id
  - changing the serialization order
  - changing enum semantics that the client must understand
```

### 2.6 Recommended Decision Flow

```
1. First do canonicalization
   - avoid changing the digest merely by reordering text

2. Then do a Merkle diff
   - quickly locate the changed subtree

3. Finally do a semantic compatibility check
   - determine backward / forward / incompatible
```

### 2.7 theseed's Trade-offs

```
theseed should not remain at the KBEngine / BigWorld style of "single digest comparison".

A more reasonable boundary is:
  - externally, the root digest can still be exposed as a fast handshake summary
  - internally, a Merkle tree is used for diff, caching, and incremental distribution
  - the final compatibility conclusion is given by the schema / protocol rule engine
```

```
Note:
  - "supporting UE5 / Cocos" is now a product direction, not an MVP delivery definition
  - multi-platform generation should not block the Runtime Core and the Unity main chain
```

### Unity / C# Example

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

### Network Layer

```csharp
public class NetworkClient {
    public async Task Connect(string host, int port);
    public async Task ConnectWs(string url);
    public async Task<AuthResult> Login(string account, string token);
    public void EnableAutoReconnect(ReconnectConfig config);
}
```

### Entity Sync Engine

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

### Interpolation Strategy

```
Property type    Interpolation strategy
position         linear interpolation + client prediction
rotation         spherical interpolation (SLERP)
hp/mp            linear interpolation
level/name       no interpolation (direct jump)
```

---

## 4. Exposed Method Security

```
Trust boundary:
  - only methods with exposed=true generate a client CallServer function
  - the server automatically validates: type check + range check + rate limiting
  - non-exposed methods do not appear in client-generated code
```

---

## 5. Bandwidth Optimization

```
1. detail_level tiered synchronization
2. alias mechanism (1 byte instead of the entity type string)
3. property delta (send only changed properties)
4. message merging (merge multiple changes within a tick into a single message)
```

---

## 6. Phase 2 Direction

```
UE5 direction:
  - code generation
  - Blueprint-callable API
  - entity-synchronization semantics aligned with Unity

Cocos direction:
  - TypeScript code generation
  - lightweight runtime SDK
  - WebSocket-first integration
```

```
But before Phase 2, no commitment is made to:
  - deep UE5 Blueprint integration
  - equivalent editor tooling across the three platforms
  - fully isomorphic network stacks across the three platforms
```

---

## 7. Comparison with KBEngine

| Capability | KBEngine | theseed |
|------|---------|---------|
| Unity | plugin + offline-generated SDK + runtime import | MVP main support |
| UE5 | none | Phase 2 target |
| Cocos | none | Phase 2 target |
| Protocol generation | partially exists (client SDK generation chain) | theseed-codegen |
| Property interpolation | none | InterpolatedProperty |
| Reconnect after disconnection | limited | auto-reconnect + server keep-alive |
| SDK update chain | limited but exists | Phase 2 reserve |
