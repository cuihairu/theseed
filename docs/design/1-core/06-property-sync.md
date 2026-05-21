# Property Sync — 属性复制与同步

> 属性同步是 theseed 最核心的数据流：实体属性变更如何高效传播到所有相关方。
>
> 来源：BigWorld DataLoDLevels + VolatileInfo，KBEngine onDefDataChanged + Witness::update。
> 当前实现基线以 [0-foundation/01-mvp-architecture-baseline](../0-foundation/01-mvp-architecture-baseline.md) 为准。

---

## 0. MVP 同步总则

```
MVP 下的统一规则：

  属性变更
    → 只标记 dirty
    → 不在脚本执行中立即外发
    → tick 末统一构造同步 Bundle
    → tick 末统一 flush

例外只有：
  - 明确的 RPC / 事件消息
  - 生命周期控制消息（创建 / 销毁 / 迁移控制）
```

---

## 1. 同步方向

```
属性同步有四条路径：

Path 1: Real → Ghost（CellApp 内部同步）
  触发：属性变更 onDefDataChanged
  条件：isReal() && hasGhost() && 属性标记 CELL_PUBLIC
  方式：标记 ghost dirty，tick 末统一构造 state-delta Bundle

Path 2: Real → Witness（服务端到客户端）
  触发：属性变更 onDefDataChanged
  条件：属性标记 OTHER_CLIENTS 或 OWN_CLIENT
  方式：
    OWN_CLIENT: 标记 own-client dirty
    OTHER_CLIENTS: 标记 witness dirty
    注意：MVP 下不在属性 setter 中直接发客户端消息

Path 3: Witness → Client（tick 末批量）
  触发：Witness::tick()
  内容：
    - 视野进出事件
    - 位置/朝向 volatile 更新
    - detailLevel 变更
    - 脏属性按 detailLevel 过滤后发送
  方式：一个 Bundle 包含所有更新，一次发送

Path 4: Entity → Database（持久化）
  触发：setDirty() 标记 + 定时 save
  条件：属性标记 Persistent
  方式：定时（如 30s）将所有脏属性序列化写库
```

---

## 2. 脏标记系统

```cpp
// runtime/DirtyMask.h

// 使用位图标记哪些属性变更了
// uint64 可覆盖 64 个属性，超过则用 vector<uint64>

class DirtyMask {
public:
    void mark(PropertyId id) {
        mask_ |= (1ULL << id);
    }

    bool isDirty(PropertyId id) const {
        return (mask_ & (1ULL << id)) != 0;
    }

    bool any() const { return mask_ != 0; }

    void clear() { mask_ = 0; }

    template<typename Func>
    void foreachDirty(Func&& fn) const {
        uint64_t m = mask_;
        PropertyId id = 0;
        while (m) {
            if (m & 1) fn(id);
            m >>= 1;
            id++;
        }
    }

private:
    uint64_t mask_ = 0;
};
```

```
说明：
  - PropertyBlock 的 dirty 只表示“本 tick 内变更过”
  - 它不等价于“已经发给所有观察者”
  - 不同目标（ghost / witness / db）可以有各自的发送游标或过滤逻辑
```

---

## 3. Sync Build & Flush

```
tick 末统一构造同步 Bundle 的逻辑：

1. 收集本 tick 的生命周期事件
   - create / leave / destroy
   - detailLevel 变更

2. 构造 Ghost state-delta Bundle
   - 只序列化 ghost 可见属性
   - 按实体和属性脏位图输出

3. 构造 Witness / Client Bundle
   - 先输出视野进出
   - 再输出属性 delta
   - 最后附加 volatile 数据

4. 统一 flush
   - Runtime Data Plane: real → ghost
   - Client Channel: witness → client

说明：
  onDefDataChanged 的职责是“标脏”，不是“立即发包”。

Bundle layout:
┌─────────────────────────────────────────────────────────┐
│ Header: 消息 ID (aliasID 或 utype)                       │
├─────────────────────────────────────────────────────────┤
│ Entity 1:                                               │
│   entityID (aliasID 如果可用)                             │
│   detailLevel 变更标志                                    │
│   ┌─ 脏属性 bitmap (只包含当前 detailLevel 的属性)         │
│   │  属性值序列化...                                       │
│   └─                                                     │
│   volatile 数据 (位置/朝向)                                │
├─────────────────────────────────────────────────────────┤
│ Entity 2: ...                                           │
├─────────────────────────────────────────────────────────┤
│ Tail: 结束标记                                           │
└─────────────────────────────────────────────────────────┘
```

带宽优化手段（来自 KBEngine）：

1. aliasID: 属性数 < 255 时用 1 字节代替 2 字节 utype
2. detailLevel: 远处实体只同步部分属性
3. 脏属性 bitmap: 只发送变化的属性
4. volatile threshold: 位置/朝向变化超过阈值才同步
5. entity alias: 首次发送 entity type string，后续用 1 字节 alias
