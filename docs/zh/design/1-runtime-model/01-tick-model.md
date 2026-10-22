# Tick Model — Main Loop Model

> English | **中文**

> The main loop design of the theseed engine. All game logic, network processing, and property synchronization are executed inside the tick.
>
> Origin: the BigWorld main loop and process-level parallelism model.
> Reference implementation: the lightweight tick approach of KBEngine's `EventDispatcher::processUntilBreak`.
> The current implementation baseline follows [0-foundation/01-mvp-architecture-baseline](../0-foundation/01-mvp-architecture-baseline.md).

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
BigWorld centers on the main-loop tick:
  - Network packet intake
  - Logic execution
  - Property synchronization
  - Timer driving

It places parallelism mainly at the process level, not the thread level.
```

### How KBEngine Implements It

```
KBEngine also centers on a single-threaded tick:
  - EventDispatcher drives events
  - Entity logic advances serially within the tick
  - Multi-process scaling carries horizontal parallelism
```

### Pros and Cons

```
Shared pros:
  - Strong determinism
  - Low mental burden on the script side
  - Better suited to high-frequency entity state updates

Shared cons:
  - Limited parallelism within a single process
  - Complex computation must be offloaded to async or another process
```

### theseed's Trade-offs

```
theseed chooses to keep the single-threaded tick model,
because it is a runtime foundation jointly validated by the BigWorld origin design and the KBEngine reference implementation.

Differences are confined to:
  - Task splitting
  - Async boundaries
  - Separation of control plane and data plane
```

---

## 1. Core Problem

```
An entity lives in a server cluster, and it needs to:

1. Run on the correct process (Base or Cell)
2. Have a position in the correct space (Cell side)
3. Be seen by the correct clients (Witness)
4. Be perceived by neighbors across boundaries (Ghost)
5. Have property changes synchronized efficiently to all relevant parties
6. Migrate safely across processes
7. Have its state persisted and restored
```

These 7 things determine every component of the Runtime Core.

---

## 2. Why a Single-Threaded Tick

Both KBEngine and BigWorld use a **single-threaded tick model**. This is not backwardness, but the correct abstraction for game logic:

```
Reason 1: Determinism
  - Game logic must be predictable: same input → same output
  - Multi-threaded concurrent writes to the same entity would break this guarantee
  - The cost of locking is unacceptable under high-frequency ticks (10-20Hz)

Reason 2: Simplicity
  - The script layer need not care about thread safety
  - The Python GIL is itself single-threaded
  - Lowest mental burden for developers

Reason 3: Performance
  - Single-threaded means lock-free and cache-friendly
  - Game logic is usually not the CPU bottleneck (network I/O and AOI are)
  - Real parallelism comes from distribution (multi-process), not multi-threading
```

---

## 3. Tick Lifecycle

```
┌──────────────────────────────────────────────────────────────────────┐
│  One Tick (typically 10Hz → 100ms budget)                            │
│                                                                      │
│  1. Network Ingress                                                  │
│     ├─ Receive all ready messages                                    │
│     ├─ Deserialize into the message queue                            │
│     ├─ Deliver previous round's async task completions               │
│     └─ Dispatch in Channel / Entity order                            │
│                                                                      │
│  2. Timer                                                            │
│     ├─ Fire due timers                                               │
│     └─ Add callbacks to the current tick's execution queue           │
│                                                                      │
│  3. Entity Logic                                                     │
│     ├─ AOI node movement                                             │
│     ├─ Controller tick                                               │
│     ├─ Run C++ core logic                                            │
│     └─ Only the owning tick thread may modify entity state           │
│                                                                      │
│  4. Script Callback                                                  │
│     ├─ Execute message-triggered script callbacks                    │
│     ├─ Execute timer-triggered script callbacks                      │
│     └─ Property changes only mark dirty, never sent out immediately  │
│                                                                      │
│  5. Sync Build                                                       │
│     ├─ Collect lifecycle events                                      │
│     ├─ Build real → ghost state-delta Bundle                         │
│     ├─ Build witness → client Bundle                                 │
│     └─ Compute the flush list for this tick                          │
│                                                                      │
│  6. Flush                                                            │
│     ├─ flush Runtime Data Plane                                      │
│     ├─ flush Client Channel                                          │
│     └─ Update tick statistics and backpressure metrics               │
│                                                                      │
│  7. Idle                                                             │
│     └─ Wait for the next tick to wake up                             │
└──────────────────────────────────────────────────────────────────────┘
```

### 3.1 Key Constraints

```
MVP timing rules:

  - Modifying properties is allowed within the tick
  - Within the tick, only dirty marking is recorded; nothing is sent out
  - At the end of the tick, synchronization data is built uniformly
  - At the end of the tick, a uniform flush occurs

The only exceptions are:
  - Explicit RPC / event messages
  - Lifecycle control messages such as create / destroy / migrate
```

### 3.2 Boundary with Async Tasks

```
Async tasks may execute on background threads, but their results cannot directly write to an Entity.

Required:
  - First deliver the completion event to the owning tick thread
  - Enter the main loop at the next Network Ingress or event dispatch stage

Forbidden:
  - Calling setProperty() directly on a background thread
  - Blocking the tick thread waiting on Future.get()
```

---

## 4. Core Interfaces

```cpp
// runtime/TickModel.h

class ITickable {
public:
    virtual ~ITickable() = default;
    virtual void tick(Duration deltaTime) = 0;
};

class TickScheduler {
public:
    void registerTickable(ITickable* obj, TickPhase phase);

    void run();
    void stop();

    uint64_t currentTick() const;
    Duration tickBudget() const;
    Duration lastTickDuration() const;

private:
    enum class TickPhase {
        Network,        // 消息接收、异步完成事件投递
        Timer,          // 定时器到期
        Entity,         // C++ 核心逻辑
        Script,         // 脚本回调
        SyncBuild,      // 构造同步数据
        Flush,          // 统一发送
    };

    std::array<std::vector<ITickable*>, 6> phaseQueues_;
};
```

---

## 5. Correspondence with KBEngine

```
KBEngine:
  EventDispatcher::processUntilBreak()
    → processNetworkRequests()     → Phase::Network
    → processTimers()              → Phase::Timer
    → Cellapp::processEntityTick() → Phase::Entity
    → script execute               → Phase::Script
    → Witness::update()            → Phase::SyncBuild / Flush

theseed:
  TickScheduler::run()
    → tick(Network)
    → tick(Timer)
    → tick(Entity)
    → tick(Script)
    → tick(SyncBuild)
    → tick(Flush)
```

```
Differences:
  - theseed explicitly splits "build synchronization data" and "send" into two stages
  - This makes backpressure, statistics, retries, and debugging much clearer
  - It also avoids mixing property replication into the script execution path
```
