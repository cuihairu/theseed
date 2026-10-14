# IO Runtime & Backend Abstraction — Cross-Platform I/O Runtime and Backend Abstraction

> **English** | [中文](04-io-runtime-and-backend-abstraction.zh.md)

> Target platforms: Linux / Windows / macOS.
> The goal is not to blend `epoll / kqueue / IOCP / io_uring` into a lowest common denominator,
> but to provide a runtime abstraction high enough that Tick, Transport, Gateway, and the control plane can all reuse the same I/O lifecycle.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
BigWorld is mainly Mercury::EventDispatcher + NetworkInterface:
  - The event loop and the network interface are tightly bound
  - The file descriptor registration model is clear
  - Closer to the traditional Reactor style

Its advantage is maturity and stability,
but it is not modeled according to today's Reactor / Completion layering.
```

### How KBEngine Implements It

```
KBEngine is mainly EventDispatcher -> EventPoller -> poller_*:
  - epoll on Linux
  - select as a fallback
  - Some IOCP direction has been attempted on the Windows side

It extracts the poller more explicitly than BigWorld,
but the top-level abstraction still leans toward a "fd readiness poller",
which is unsuitable as the overarching abstraction for io_uring / IOCP.
```

### Pros and Cons

```
BigWorld / KBEngine pros:
  - The event loop is simple and direct
  - The network main path is easy to understand
  - Pairs naturally with a single-threaded tick

Shared cons:
  - The top-level abstraction sticks too closely to the select/epoll era
  - fd/register/deregister is not the ideal semantics for io_uring / IOCP
  - timer/task/wakeup/completion are not abstracted uniformly into the same runtime
```

### theseed's Trade-offs

```
theseed no longer treats EventPoller as the overarching abstraction,
but splits it into three layers:

  1. IORuntime
     Unifies run / wakeup / submit / cancel / drainCompletions

  2. Backend Family
     - ReactorBackend: select / epoll / kqueue
     - CompletionBackend: io_uring / IOCP

  3. Transport Adapter
     TCP / UDP / WebSocket / Runtime Channel are reused at the upper layer

This lets us:
  - Keep the tick-friendly main loop of BigWorld / KBEngine
  - Without forcing modern completion-style backends into poller semantics
```

---

## 0. Design Boundary

This document covers:

```
  - The I/O backend abstraction for Linux / Windows / macOS
  - The two backend semantics, Reactor and Completion
  - Startup-time capability probing and configuration decisions
  - The interface between Tick Runtime and I/O Runtime
  - The unified interface for handle / request / completion / cancel
```

This document does not cover:

```
  - EntityCall / Mailbox semantics
  - Business reliability tiers of the Runtime Data Plane
  - The business state machine of Gateway / Login
  - The MessageBus control plane
```

Related topics:

```
../2-replication-and-space/03-runtime-communication-and-transport
../5-access-and-control-plane/01-gateway-and-login
../5-access-and-control-plane/02-message-bus-and-cross-realm
```

---

## 1. Why It Can No Longer Be Called EventPoller

`EventPoller` is only suitable for expressing:

```
  - register fd for read
  - register fd for write
  - wait until ready
  - callback handling
```

This mental model is natural for:

```
select
epoll
kqueue
```

It is natural.

But not for:

```
io_uring
IOCP
```

They are unnatural, because they are closer to:

```
  - submit operation
  - operation completes later
  - consume completion
  - optionally cancel
```

So if the overarching abstraction continues to be named `EventPoller`, three problems arise:

```
1. The top-level interface is locked into fd/register semantics
2. io_uring / IOCP can only be "disguised as readiness"
3. timer/task/wakeup/completion cannot be merged into the same runtime
```

Conclusion:

```
EventPoller may be kept as the implementation name of one class of backend,
but it can no longer serve as theseed's overarching abstraction.
```

---

## 2. Recommended Overarching Abstraction

Suggested name for the overarching abstraction:

```
IORuntime
```

It expresses:

```
"A runtime that drives I/O requests, waits for events or completions, and delivers results to the upper-layer Tick"
```

rather than:

```
"some kind of fd poller"
```

### 2.1 Core Interfaces

```cpp
enum class IoOp : uint8_t {
    Accept,
    Connect,
    Read,
    Write,
    RecvFrom,
    SendTo,
};

struct IoHandle {
    uint64_t value = 0;
    uint32_t generation = 0;
};

struct IoBuffer {
    void* data = nullptr;
    uint32_t size = 0;
};

struct IoRequest {
    IoOp op;
    IoHandle handle;
    IoBuffer buffer;
    void* userData = nullptr;
};

struct IoToken {
    uint64_t value = 0;
};

enum class IoStatus : uint8_t {
    Ok,
    Cancelled,
    Timeout,
    Closed,
    Error,
};

struct IoCompletion {
    IoToken token;
    IoStatus status = IoStatus::Ok;
    uint32_t bytesTransferred = 0;
    int32_t osError = 0;
    void* userData = nullptr;
};

class IIORuntime {
public:
    virtual ~IIORuntime() = default;

    virtual void runOnce(Duration maxWait) = 0;
    virtual void wakeup() = 0;

    virtual IoToken submit(const IoRequest& request) = 0;
    virtual bool cancel(IoToken token) = 0;

    virtual size_t drainCompletions(IoCompletion* out, size_t capacity) = 0;
};
```

The most important change here is:

```
The unified semantics are submit / cancel / completion,
not registerRead / registerWrite / triggerRead.
```

---

## 3. The Two Backend Families

### 3.1 ReactorBackend

Applicable to:

```
  - select
  - epoll
  - kqueue
```

Semantics:

```
  - The backend reports that a "handle is ready"
  - The runtime then translates "readiness" into an IoRequest that can be advanced further
```

### 3.2 CompletionBackend

Applicable to:

```
  - io_uring
  - IOCP
```

Semantics:

```
  - A concrete I/O operation is submitted first
  - The backend produces completion events directly
  - The runtime consumes the completion queue
```

### 3.3 Why Not Hard-Code Proactor

Strictly speaking:

```
IOCP is more typically close to Proactor
io_uring can do both poll and completion
```

So the more accurate naming in the documentation should be:

```
Reactor / Completion
```

rather than dogmatically writing only:

```
Reactor / Proactor
```

---

## 4. Startup-Time Backend Selection

Backend selection is only allowed at startup time; hot switching at runtime is not allowed.

### 4.1 Selection Flow

```
1. Probe system capabilities
2. Read the configuration policy
3. Select the optimal backend
4. Emit the final decision log
5. Initialize the corresponding runtime
```

### 4.2 Capability Probing

Must probe:

```
  - OS type
  - Whether the kernel / API supports the target backend
  - Whether current permissions or resource limits allow enabling it
```

Example:

```
Linux:
  - Probe io_uring first
  - Fall back to epoll if unavailable

macOS:
  - kqueue

Windows:
  - IOCP
```

### 4.3 Configuration Policy

```toml
[runtime.io]
backend = "auto"
# auto
# force:io_uring
# force:epoll
# force:kqueue
# force:iocp
```

Rules:

```
auto:
  Automatically select the best available backend

force:*:
  The specified backend must be enabled
  If the system does not support it, startup fails
```

### 4.4 Selection Interfaces

```cpp
enum class IoBackendKind : uint8_t {
    Select,
    Epoll,
    Kqueue,
    IoUring,
    Iocp,
};

struct IoBackendCapability {
    bool supported = false;
    std::string reason;
};

class IIORuntimeFactory {
public:
    virtual IoBackendCapability probe(IoBackendKind kind) = 0;
    virtual std::unique_ptr<IIORuntime> create(
        IoBackendKind kind,
        const RuntimeConfig& config) = 0;
};
```

---

## 5. Relationship with Tick

theseed still keeps:

```
A single-threaded Tick Runtime
```

The I/O Runtime does not start a separate business thread model; instead it:

```
Provides input and completion events for the Tick's Network phase
```

### 5.1 Constraints on the Tick Side

```
1. All completions are consumed only on the owning tick thread
2. Background system threads may exist, but they cannot modify an Entity directly
3. Completions must first enter the runtime queue, then enter the Network phase
```

### 5.2 Recommended Sequence

```text
Tick::NetworkPhase
  ├─ ioRuntime.runOnce(maxWait=0)
  ├─ ioRuntime.drainCompletions(...)
  ├─ translate completion -> packet / channel event
  └─ dispatch to runtime message queue
```

If some backends need helper threads internally:

```
They are allowed to exist,
but these threads do not own Entities, nor do they enter the script layer directly.
```

---

## 6. Separation of Handle / Request / Completion

This layer of abstraction must avoid exposing `fd` as a core concept of the upper layers.

### 6.1 Why Not Just Expose fd

```
1. Windows is not naturally equivalent to Unix fd semantics
2. The focus of IOCP / io_uring is not fd readiness
3. If TLS, KCP, or WebSocket wrapping is added later, a logical handle is more stable
```

### 6.2 Recommended Object Hierarchy

```
IoHandle
  Represents the identity of a low-level handle managed by the runtime

IoRequest
  Represents a single pending I/O operation

IoCompletion
  Represents the result of a single completed I/O operation
```

### 6.3 Cancellation Semantics

It must be made explicit:

```
cancel(token) only guarantees a "best-effort cancellation"
It does not guarantee that no completion will be received after the call returns
```

Therefore the upper layers must accept:

```
Completions may arrive late
Completions must carry a token / generation
The upper layers perform idempotent filtering by epoch / handle generation
```

---

## 7. Interface with Transport

Transport should not depend directly on a concrete backend,
but on:

```
IIORuntime
```

### 7.1 Transport Adapter Layer

```cpp
class ITransportEndpoint {
public:
    virtual ~ITransportEndpoint() = default;

    virtual void start(IIORuntime& ioRuntime) = 0;
    virtual void stop() = 0;

    virtual void enqueueSend(PacketSpan packet) = 0;
    virtual void onCompletion(const IoCompletion& completion) = 0;
};
```

This way:

```
TCP / UDP / WebSocket / Runtime Channel
```

all face only the unified completion semantics.

### 7.2 The Place of the Legacy poller

If compatibility with legacy-style implementations is needed, it may be kept:

```cpp
class IReactorBackend {
public:
    virtual bool watchRead(IoHandle handle) = 0;
    virtual bool watchWrite(IoHandle handle) = 0;
    virtual void unwatchRead(IoHandle handle) = 0;
    virtual void unwatchWrite(IoHandle handle) = 0;
};
```

But it belongs only to:

```
Inside ReactorBackend
```

It can no longer propagate upward as an overarching interface.

---

## 8. MVP Boundary

MVP should support:

```
Linux:
  - epoll is mandatory
  - io_uring as Phase 2 or an experimental feature

Windows:
  - Establish the IOCP design boundary first
  - Implementation priority is later than the Linux epoll main path

macOS:
  - Establish the kqueue design boundary first
  - Implementation priority is later than the Linux epoll main path
```

The reasons for this are:

```
1. MVP first ensures the main platform runs
2. The documentation layer establishes the long-term abstraction first
3. Avoid spreading the risk of early io_uring implementation into all upper-layer modules
```

---

## 9. Final Relationship with BigWorld / KBEngine

### BigWorld

```
Worth borrowing:
  - The stable boundary between EventDispatcher and the main loop
  - The clear interface between NetworkInterface and upper-layer business

Not copied directly:
  - The old-style dispatcher as the overarching abstraction
```

### KBEngine

```
Worth borrowing:
  - The explicit layering of EventDispatcher -> Poller
  - The engineering reality of Linux epoll / fallback select

Not copied directly:
  - Covering IOCP / io_uring with poller semantics
```

### theseed

```
theseed's addition is not "inventing a new socket API",
but upgrading the event-loop experience of the old engines
into:

  - A unified IORuntime at the upper layer
  - A Reactor / Completion family in the middle layer
  - Platform backend selection at the lower layer
```

Only this fits the long-term support goal for Linux / Windows / macOS.
