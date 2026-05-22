# Timer & Object Pool — 定时器与内存管理

> TimerWheel 提供高性能定时回调；对象池减少高频分配开销。
>
> TimerWheel 来源：通用算法（非 BigWorld/KBEngine）。
> 对象池来源：KBEngine `OBJECTPOOL_POINT`。

---

## 0.5 引擎实现对照与取舍

### BigWorld 是怎么实现的

```
BigWorld 通常把定时与对象生命周期放进运行时主循环和管理器体系里：
  - tick 驱动定时回调
  - 组件内自管对象生命周期
  - 配合引擎级内存管理减少频繁分配
```

### KBEngine 是怎么实现的

```
KBEngine 更强调可落地的工程做法：
  - 事件分发驱动定时逻辑
  - 通过对象池降低高频分配成本
  - 让脚本和实体层尽量少感知底层分配细节
```

### 优缺点

```
共同优点：
  - 适合高频、短生命周期对象
  - 运行期开销可控

共同缺点：
  - 需要谨慎处理泄漏和悬挂引用
  - 复杂生命周期调试成本高
```

### theseed 的取舍

```
theseed 用 TimerWheel + 对象池，
是为了把“高频定时”和“高频分配”拆成两个独立可审计的基础设施，
而不是继续把它们隐含在实体实现里。
```

---

## 1. 定时器模型

```
游戏服务器的定时器需求：
  1. 高频（每秒数十万次触发）
  2. 精度要求不高（10ms 级别即可）
  3. 主要用于 tick 驱动的回调
  4. 支持一次性 / 周期性
  5. 支持取消

经典方案：时间轮（Hashed Timing Wheel）
  - O(1) 添加/取消
  - 精度取决于 wheel 的 tick duration
  - 适合大量定时器场景
```

---

## 2. 核心接口

```cpp
// runtime/TimerWheel.h

class TimerWheel {
public:
    // 创建定时器
    TimerHandle addTimer(Duration interval,
                         TimerCallback callback,
                         bool periodic = true);

    // 取消定时器
    void cancelTimer(TimerHandle handle);

    // tick 推进
    void advance(Duration dt);

    // 统计
    size_t activeTimerCount() const;

private:
    static constexpr int WHEEL_BITS = 8;
    static constexpr int WHEEL_SIZE = 1 << WHEEL_BITS;  // 256
    static constexpr Duration TICK_DURATION = 10ms;

    struct TimerEntry {
        Duration deadline;
        Duration interval;
        TimerCallback callback;
        bool periodic;
        bool cancelled;
    };

    std::array<std::vector<TimerEntry>, WHEEL_SIZE> wheels_[4]; // 4 级时间轮
    std::unordered_map<TimerId, TimerEntry> timers_;
};
```

---

## 3. 与 KBEngine 的对比

```
KBEngine:
  ScriptTimers（时间堆）
  → 按到期时间排序的优先队列
  → 添加 O(log n)，取消 O(log n)

theseed:
  TimerWheel（4 级时间轮）
  → 添加 O(1)，取消 O(1)
  → 适合大量定时器（百万级）
  → 精度 10ms（游戏服务器足够）
```
