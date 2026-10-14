# Runtime Communication & Transport — EntityCall, the Runtime Data Plane, and Transport Semantics

> **English** | [中文](03-runtime-communication-and-transport.zh.md)

> Origin: BigWorld `Mailbox / Mercury / Channel / Bundle`,
> Reference implementation: KBEngine `EntityCall / TCP`,
> theseed adds a clearer runtime data plane boundary on top of this.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
The core of BigWorld's runtime communication is Mercury / Mailbox / Channel / Bundle:
  - Expresses entity routing through channels and mailboxes
  - Runtime messages are strongly bound to connection state
  - Channel carries inactivity, RTT, encryption, statistics, and send state
  - Bundle carries message aggregation, request/reply, and reliability roles
  - Suitable for high-frequency entity main paths
```

### How KBEngine Implements It

```
KBEngine is more direct:
  - EntityCall + TCP direct connection
  - Fewer runtime semantics
  - Easier to understand, but the system-layer abstraction is also thinner
```

### Pros and Cons

```
BigWorld / KBEngine advantages:
  - The data plane is naturally close to entity routing
  - It runs without introducing an additional bus

Common disadvantages:
  - The control plane, async plane, and data plane easily get mixed together
  - Later expansion of cross-Realm coordination becomes awkward
```

### theseed's Trade-offs

```
theseed continues to separate the Runtime Data Plane from the control plane.
The data plane takes BigWorld's Channel / Bundle state semantics as its upper bound,
takes KBEngine's EntityCall direct connection path as the MVP reference,
but does not let control/ops messages mix into the entity main path.
```

### Why the Layers Cannot Continue to Be Mixed

```
Once the Runtime Data Plane and the Control Plane are mixed into one layer,
three kinds of errors quickly appear:
  1. Treating EntityCall as a general-purpose RPC
  2. Stuffing an MQ hop into the in-tick main path
  3. Mixing ops broadcasts and entity ordered messages under the same backpressure semantics
```

---

## 0. Design Boundary

This document is responsible for:

```
  - The essence of EntityCall
  - The responsibilities of the Runtime Data Plane
  - Runtime delivery semantics
  - System boundaries such as piggyback / overflow / inactivity
  - Transport implementation selection and abstraction interface
```

This document is not responsible for:

```
  - MessageBus control plane semantics
  - Cross-Realm async bridging
  - Login access traffic
```

The latter two are placed uniformly in:

```
../5-access-and-control-plane
```

---

## 1. First Separate the Three Message Planes

One of the most important boundaries in the current theseed documentation is separating the three message planes:

### 1.1 Runtime Data Plane

Responsible for:

```
  - EntityCall
  - real → ghost
  - migration snapshot
  - runtime create / destroy / route update
```

Requirements:

```
  - Clear ordering semantics
  - Entity routing is perceptible
  - Has backpressure capability
```

### 1.2 Control Plane

Responsible for:

```
  - Process online/offline
  - Routing table publication
  - Configuration broadcast
  - Controlled shutdown / ops commands
```

### 1.3 Cross-Realm Async Plane

Responsible for:

```
  - Cross-server queries
  - Cross-server notifications
  - Matchmaking / mail / async orchestration
```

So the conclusion is:

```
EntityCall does not go through MessageBus.
```

This is not an implementation preference, but an engine boundary.

---

## 2. The Real Semantics of EntityCall

EntityCall is not a general-purpose RPC framework, but rather:

```
"a proxy handle to the authoritative instance of some entity in another process"
```

### 2.1 The Handle Model

```cpp
class EntityCall {
public:
    EntityCall(EntityId id, ComponentId target, const std::string& entityType);

    void call(const std::string& method, const Args& args);
    void callWithCallback(const std::string& method,
                          const Args& args,
                          CallbackPtr callback);

    ComponentId targetComponent() const;
    void updateTarget(ComponentId newTarget);

    bool isValid() const;
    const Address& address() const;
};
```

### 2.2 Why It Cannot Go Through MQ

```
1. Calls on the same entity have ordering dependencies
2. Routing switches dynamically during migration
3. The in-tick path does not allow an extra broker hop
4. Runtime backpressure must return to the engine itself
```

This is also the common system premise of BigWorld's `Mailbox / Mercury` and KBEngine's `EntityCall`.

Conversely:

```
If a message does not need:
  - Entity authoritative routing
  - In-tick ordering dependencies
  - Migration window consistency

Then it most likely does not belong to the Runtime Data Plane.
```

---

## 3. Runtime Delivery Semantics

### 3.1 The MVP Only Commits to Two Business-Visible Semantics

```cpp
enum class DeliveryClass : uint8_t {
    ORDERED_RELIABLE = 0,
    UNORDERED_LOSSY  = 1,
};
```

Scope of application:

```
ORDERED_RELIABLE:
  - EntityCall
  - create / destroy
  - migration
  - ghost core state

UNORDERED_LOSSY:
  - volatile position / orientation
  - Auxiliary updates that can be overwritten by newer state
```

### 3.2 This Does Not Mean BigWorld Has Only Two Levels

BigWorld's real system area also includes:

```
driver / passenger / critical / none
piggyback
overflow
inactivity
channel / bundle backpressure and disconnect semantics
```

This round of reorganization keeps these boundaries in the same document, rather than continuing to scatter them across two directories.

---

## 4. BigWorld-Level Reliability Boundaries

### 4.1 piggyback

`piggyback` is not just "carrying a bit of data along the way", but rather:

```
A system semantics that reuses acknowledgments on an existing message flow, attaches small payloads, and reduces the cost of extra packets
```

### 4.2 overflow

`overflow` is not a log event, but rather:

```
When the send window, queue, or bundle capacity is exhausted,
the runtime must enter an explicit discard, degradation, or retry path.
```

### 4.3 inactivity

`inactivity` is not a simple timeout log either; it means:

```
The liveness state of a connection or channel has already affected routing, recovery, or reconnection decisions.
```

In BigWorld, none of these boundaries are "transport library implementation details".

In theseed they must also continue to be preserved as runtime semantics.

---

## 5. Transport Abstraction

### 5.1 Interface

```cpp
class IRuntimeTransport {
public:
    virtual ~IRuntimeTransport() = default;

    virtual void send(const Address& target,
                      const Message& msg,
                      DeliveryClass deliveryClass) = 0;

    virtual void registerHandler(MessageId id,
                                 MessageHandler handler) = 0;

    virtual Channel* getChannel(const Address& addr) = 0;
    virtual void closeChannel(const Address& addr) = 0;

    virtual void tick() = 0;

    virtual TransportStats getStats() const = 0;
};
```

### 5.2 Recommended Implementation

theseed still recommends:

```
MVP:
  - The Runtime Data Plane goes uniformly through IRuntimeTransport
  - The default implementation may prioritize evaluating Aeron

Alternatives:
  - TCPTransport for simplified deployment
```

The reason is not "pursuing new technology", but rather:

```
1. The same-machine IPC path has value
2. Publication backpressure is observable
3. The runtime can decide lossy merging and degradation on its own
```

---

## 6. The System Role of Channel / Bundle

In BigWorld / KBEngine, `Channel / Bundle` has never been just a serialization buffer.

What they express is:

```
  - Ordering ownership
  - Window and send cadence
  - piggyback position
  - overflow / retry decision points
```

So the theseed documentation should not describe them as:

```
A thin "message packet object"
```

A more reasonable formulation is:

```
Bundle = runtime message aggregation unit
Channel = the stateful send context to some target route
```

---

## 7. Relationship with Migration, HA, and Lifecycle

### 7.1 With Migration

The migration window needs:

```
  - An epoch identifiable before and after the route switch
  - A clear handling policy for unfinished messages
  - Messages on the old route can be forwarded, rejected, or discarded
```

### 7.2 With HA

HA cares not only about "the snapshot exists", but also about:

```
When the primary instance fails,
which runtime channel state needs to be rebuilt,
which unfinished messages can be replayed,
and which must be compensated for by upper-layer semantics.
```

### 7.3 With Lifecycle

During `drain / retire / controlled shutdown`:

```
The runtime transport also needs to enter a controlled convergence state,
rather than continuing to accept new load indefinitely as it does during the stable period.
```

---

## 8. Phased Boundary

```
MVP:
  - EntityCall goes uniformly through the Runtime Data Plane
  - Two business-visible semantics: ORDERED_RELIABLE / UNORDERED_LOSSY
  - Basic backpressure and statistics

Phase 2:
  - A clearer channel / bundle state machine
  - Explicit policy interfaces for piggyback / overflow / inactivity
  - Tighter linkage with migration / lifecycle

Phase 3:
  - A complete reliability role model closer to BigWorld's
  - More mature runtime degradation and recovery strategies
```

---

## 9. Comparison with BigWorld / KBEngine / theseed

| Dimension | BigWorld | KBEngine | theseed |
|------|------|------|------|
| Entity remote handle | Mailbox | EntityCall | EntityCall |
| Main-path message plane | Mercury direct connection | TCP direct connection | Runtime Data Plane |
| MQ carries the main path | No | No | No |
| Reliability system area | Complete and fine-grained | Relatively simplified | MVP converges, boundaries retained |
| piggyback / overflow / inactivity | Yes | Weak | Explicitly written into the design boundary |

---

## 10. One-Sentence Judgment

The goal of this document is not to prove:

```
theseed has already equivalently implemented Mercury
```

But rather to make clear:

```
Runtime communication must be modeled as part of the "Replication and Space" layer,
and the BigWorld-style reliability system area has already been established independently within the documentation boundary.
```
