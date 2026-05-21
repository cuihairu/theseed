# Timer & Object Pool — 定时器与内存管理

> TimerWheel 提供高性能定时回调；对象池减少高频分配开销。
>
> TimerWheel 来源：通用算法（非 BigWorld/KBEngine）。
> 对象池来源：KBEngine `OBJECTPOOL_POINT`。

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
