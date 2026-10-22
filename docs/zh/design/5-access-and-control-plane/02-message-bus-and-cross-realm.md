# MessageBus — Message Bus and Cross-Realm

> English | **中文**

> theseed's MessageBus is responsible for the Control Plane and the Cross-Realm Async Plane.
> The Runtime Data Plane is handled by the separate Runtime Transport and does not go through MessageBus.
>
> Origin judgment: neither BigWorld nor KBEngine has a modern general-purpose MessageBus.
> theseed adds message bus capabilities for the Control Plane and the Cross-Realm Async Plane.
> The current implementation baseline follows [../0-foundation/01-mvp-architecture-baseline](../0-foundation/01-mvp-architecture-baseline.md).

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
BigWorld has no independent general-purpose MessageBus layer.

Its approach is:
  - runtime main path: Mercury / Mailbox / Channel
  - ops control plane: Watcher / ForwardingWatcher
  - cross-process management: Manager process + built-in message protocol
  - cross-realm async: no modern MQ-style abstraction
```

### How KBEngine Implements It

```
KBEngine also has no independent message bus.

Its approach is:
  - runtime main path: direct TCP + EntityCall
  - control and management: custom messages between components
  - ops observation: Watcher / WebConsole
  - cross-realm async: essentially no systematic MessageBus
```

### Pros and Cons

```
BigWorld / KBEngine's advantages:
  - the main path and control-plane boundaries are naturally clear
  - few components, low deployment burden
  - will not mistakenly put MQ on the EntityCall main path

BigWorld / KBEngine's drawbacks:
  - control-plane broadcast, cross-realm bridging, and background tasks all have to be assembled by hand
  - lack of a unified request-reply / pub-sub / worker group abstraction
  - weak cross-datacenter and async orchestration capability
```

### theseed's Trade-offs

```
theseed does not follow the old engines' approach of "everything goes through custom point-to-point".

The trade-off is:
  - Runtime Data Plane stays independent and does not go through MessageBus
  - Control Plane / Cross-Realm Async Plane introduce MessageBus separately
  - use external components in exchange for broadcast, bridging, async orchestration, and ops manageability
```

### Why Not Aeron

```
Aeron is stronger at low-latency transport and streaming data planes,
but it is not an out-of-the-box distributed message bus:
  - requires more self-built protocol and control plane
  - better suited to the Data Plane
  - not suited to directly carrying control-plane semantics
```

### Why Not NNG

```
NNG is more like a messaging/transport toolbox
than a ready-made distributed message bus:
  - suited to point-to-point communication
  - not good at replacing a complete bus / pub-sub / request-reply hub
```

### Why Not Develop It In-House Directly

```
Developing the bus in-house would swallow ops, discovery, reconnection, routing, and permissions all at once,
and the cost is usually higher than using a mature component first.
```

---

## 1. Why a Message Bus Is Needed

```
A world without a message bus:
  control-plane and cross-realm capabilities both rely on point-to-point connections
  discovery, broadcast, and bridging quickly get out of control

A world with a message bus:
  Control Plane / Cross-Realm Async Plane have a unified entry point

Extra benefits:
  - process discovery and broadcast are simple
  - cross-realm messages are naturally supported
  - the request/response pattern is unified
  - async task distribution is clearer

Note:
  MessageBus does not replace Runtime Transport.
  The entity main path is still handled by the Runtime Data Plane.
```

---

## 2. Backend Selection

**Under MVP theseed chooses NATS as the default MessageBus backend**:

| Candidate | Fit | Main Reason |
|------|------|------|
| NATS | High | official native support for pub/sub, request-reply, and queue groups; suited to the control plane and cross-realm async |
| Aeron | Medium | official positioning is low-latency transport, Archive, and Cluster; more like a data plane / transport layer |
| NNG | Medium-low | official positioning is a protocols + transports toolbox, not a ready-made distributed bus |
| In-house development | Low | discovery, routing, reconnection, permissions, and the ops plane all have to be built in-house |

The reason for ultimately choosing NATS is not "lowest-level and fastest", but rather:

- The runtime main path is already solved by Runtime Transport
- MessageBus cares more about subject, request-reply, queue group, and cross-datacenter bridging
- NATS fits the needs better on the control plane and cross-realm async
- Its ops and deployment model is also more mature
- NATS's official documentation explicitly lists request-reply and queue groups as core capabilities

The boundaries are as follows:

1. Runtime Transport
   - EntityCall
   - Ghost / Witness synchronization
   - Migration data

2. MessageBus
   - Control plane messages
   - Cross-Realm async calls
   - Background task distribution
   - Broadcast / notification / offline queue

Supplementary judgment:

```
Aeron is better suited to low-latency transport or as the lower layer of a bus, and is not suited to directly carrying control-plane semantics.
NNG is better suited as a protocol and transport toolbox, and is not suited to directly replacing a distributed messaging hub.
```

---

## 3. Core Interfaces

```cpp
class IMessageBus {
public:
    // 发布/订阅
    virtual void publish(const std::string& subject,
                         const std::vector<uint8_t>& data) = 0;
    virtual Subscription subscribe(const std::string& subject,
                                   MessageCallback callback) = 0;

    // 请求/响应
    virtual Future<BusResponse> request(const std::string& subject,
                                        const std::vector<uint8_t>& data,
                                        Duration timeout = 5s) = 0;

    // 定向发送
    virtual void send(const std::string& targetProcess,
                      const std::string& subject,
                      const std::vector<uint8_t>& data) = 0;

    // 集群广播
    virtual void broadcast(ProcessRole role,
                           const std::string& subject,
                           const std::vector<uint8_t>& data) = 0;

    virtual bool isConnected() const = 0;
    virtual BusStats getStats() const = 0;
};
```

---

## 4. Message Uses

```
MessageBus uses
├── control-plane messages
│   ├── process online/offline notification
│   ├── routing table update
│   ├── configuration change broadcast
│   └── hot update command dispatch
├── async tasks
│   ├── background task distribution
│   ├── offline notification
│   └── audit log
└── cross-realm messages
    ├── matchmaking
    ├── query
    ├── mail / notification
    └── Bridge forwarding
```

```
MessageBus must not carry:
  - EntityCall main path
  - real → ghost state synchronization
  - migration snapshot data
  - in-tick real-time AOI / property replication
```

---

## 5. Cross-Realm Bridging

```
Realm A                           Realm B
┌─────────────────┐              ┌─────────────────┐
│ NATS Cluster A  │◄── Bridge ──►│ NATS Cluster B  │
│ BaseApp_1       │              │ BaseApp_1       │
│ CellApp_1       │              │ CellApp_1       │
└─────────────────┘              └─────────────────┘

Bridge configuration:
  - only forward messages with the cross-realm prefix
  - OTLP compatible (trace context propagation)
  - supports request/response
  - does not carry the entity's local authority path
```

### Cross-Realm Script API

```python
import theseed

class Avatar(theseed.BaseEntity):
    def startCrossMatch(self):
        theseed.cross_server.match(
            mode="pvp_3v3",
            player_info={"uid": self.id, "mmr": self.mmr},
            callback=self.onMatchResult
        )

    def checkFriendOnline(self, friend_uid):
        result = yield theseed.cross_server.query(
            realm="realm_b",
            method="Player.checkOnline",
            args={"uid": friend_uid}
        )

    def sendCrossServerMail(self, target_realm, target_uid, mail):
        theseed.cross_server.send_message(
            realm=target_realm,
            method="Mail.receive",
            args={"uid": target_uid, "mail": mail},
            guaranteed=True
        )
```

### CrossServerManager

```cpp
class CrossServerManager {
public:
    Future<CrossResult> call(const std::string& realm, ...);
    Future<MigrationResult> migrateEntity(EntityId id, ...);
    void sendMessage(const std::string& realm, ...);
    Future<QueryResult> query(const std::string& realm, ...);
    std::vector<RealmInfo> discoverRealms(const std::string& service);
};
```

### Realm Bridge

```
Responsibilities:
  1. message format conversion
  2. Trace Context propagation
  3. message persistence
  4. authentication
  5. flow control

Implementation:
  same datacenter: NATS Leafnode
  cross datacenter: NATS Gateway or a custom TCP bridge
```
