# Timer & Object Pool — Timers and Memory Management

> **English** | [中文](03-timer-and-memory.zh.md)

> TimerWheel provides high-performance timer callbacks; object pools reduce high-frequency allocation overhead.
>
> TimerWheel origin: a general algorithm (not BigWorld/KBEngine).
> Object pool origin: KBEngine `OBJECTPOOL_POINT`.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
BigWorld usually places timing and object lifecycle into the runtime main loop and the manager hierarchy:
  - The tick drives timer callbacks
  - Components manage object lifecycles themselves
  - Engine-level memory management reduces frequent allocation
```

### How KBEngine Implements It

```
KBEngine emphasizes implementable engineering practices:
  - Event dispatch drives timing logic
  - Object pools reduce the cost of high-frequency allocation
  - The script and entity layers are kept as unaware as possible of low-level allocation details
```

### Pros and Cons

```
Shared pros:
  - Suited to high-frequency, short-lived objects
  - Controllable runtime overhead

Shared cons:
  - Leaks and dangling references must be handled carefully
  - Debugging complex lifecycles is costly
```

### theseed's Trade-offs

```
theseed uses TimerWheel + object pools
to split "high-frequency timing" and "high-frequency allocation" into two independent, auditable pieces of infrastructure,
rather than continuing to leave them implicit inside the entity implementation.
```

---

## 1. Timer Model

```
Timer requirements of a game server:
  1. High frequency (hundreds of thousands of triggers per second)
  2. Low precision requirements (10ms level is sufficient)
  3. Mainly used for tick-driven callbacks
  4. Support for one-shot / periodic
  5. Support for cancellation

Classic approach: the timing wheel (Hashed Timing Wheel)
  - O(1) add/cancel
  - Precision depends on the wheel's tick duration
  - Suited to scenarios with a large number of timers
```

---

## 2. Core Interfaces

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

## 3. Comparison with KBEngine

```
KBEngine:
  ScriptTimers (time heap)
  → priority queue ordered by expiry time
  → add O(log n), cancel O(log n)

theseed:
  TimerWheel (4-level time wheel)
  → add O(1), cancel O(1)
  → suitable for large numbers of timers (millions)
  → 10 ms precision (sufficient for game servers)
```
