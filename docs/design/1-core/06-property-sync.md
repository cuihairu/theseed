# Property Sync — 属性复制与同步

> 属性同步是 theseed 最核心的数据流：实体属性变更如何高效传播到所有相关方。
>
> 来源：BigWorld DataLoDLevels + VolatileInfo，KBEngine onDefDataChanged + Witness::update。

---

## 1. 同步方向

```
属性同步有四条路径：

Path 1: Real → Ghost（CellApp 内部同步）
  触发：属性变更 onDefDataChanged
  条件：isReal() && hasGhost() && 属性标记 CELL_PUBLIC
  方式：立即发 Bundle 给 ghost CellApp

Path 2: Real → Witness（服务端到客户端）
  触发：属性变更 onDefDataChanged
  条件：属性标记 OTHER_CLIENTS 或 OWN_CLIENT
  方式：
    OWN_CLIENT: 直接发给自己客户端
    OTHER_CLIENTS: 遍历 witnesses 发送
    注意：非 volatile 属性是立即发送，不是 tick 末批量

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

    // 遍历所有脏属性
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

---

## 3. 同步 Bundle 构造

```
tick 末 Witness::tick() 构造同步 Bundle 的逻辑：

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

带宽优化手段（来自 KBEngine）：
  1. aliasID: 属性数 < 255 时用 1 字节代替 2 字节 utype
  2. detailLevel: 远处实体只同步部分属性
  3. 脏属性 bitmap: 只发送变化的属性
  4. volatile threshold: 位置/朝向变化超过阈值才同步
  5. entity alias: 首次发送 entity type string，后续用 1 字节 alias
```
