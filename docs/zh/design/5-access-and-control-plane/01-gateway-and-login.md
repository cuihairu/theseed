# Gateway & Login — Access Layer, Login Service, and the Challenge Ops Boundary

> English | **中文**

> Origin: BigWorld `LoginApp / Login Challenge / statusCheck / clearIPAddressBans`.
> Reference implementation: the lightweight access path of KBEngine's `LoginApp`.
> theseed separates the responsibilities of Gateway and Login Service on this basis.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
BigWorld's LoginApp is not merely an access point:
  - challenge
  - pending request
  - IP ban management
  - status check
  - controlled shutdown coordination

It is essentially an independent ops subsystem.
```

### How KBEngine Implements It

```
KBEngine also has a LoginApp,
but it leans more toward access and session distribution,
and its ops control plane is not as thick as BigWorld's.
```

### Implementation Capability Comparison

| Dimension | BigWorld | KBEngine | theseed trade-off |
|------|----------|----------|--------------|
| LoginApp positioning | login entry point + login state machine + ops subsystem | lightweight login entry point + session distribution | currently follows BigWorld, implementing the core login-plane services first |
| Gateway boundary | access and login-plane responsibilities are historically coupled | LoginApp takes on more access responsibilities | logically retains the Gateway/Login boundary; MVP does not force a process split |
| Challenge | Login Challenge / factory capabilities are explicit | basically does not emphasize complex challenge | currently keeps the switch and interface positions; the complex state machine is enhanced later |
| Pending request | login request state is trackable and controllable | relatively lightweight | MVP may omit the complex state machine for now, but the interface reserves an audit point |
| IP ban / risk control | temporary ban and cleanup commands built into the login plane | weaker | introduced in Phase C to avoid overloading the MVP |
| statusCheck | part of the login-plane ops commands | weaker | Phase C integrates with the unified Ops Control Plane |
| Controlled shutdown | coordinates draining / shutdown with the login entry point | weaker | Phase C implements controlledShutdown / draining |
| Realm selection | not a core abstraction | common implementations rely on LoginApp routing | currently LoginApp directly returns the realm list and character summaries |
| Realm entry routing | depends on cluster state or service discovery | lightweight distribution | currently supports a fixed ip:port per realm first; policy-driven routing goes into the pending plan |
| Session issuance | the login service completes the session handoff | LoginApp directly distributes session information | theseed uses ISessionIssuer to issue a token bound to accountId + realmId |
| Deployment form | leans toward a complete service plane | easier to land as a single point | currently lands a single LoginApp first; multiple entry points and Pipeline go into the pending plan |

### Pros and Cons

```
BigWorld's advantages:
  - login path and ops path are integrated
  - the attack-defense and shutdown boundaries are more complete

KBEngine's advantages:
  - simple
  - easier to land first

Shared drawbacks:
  - if the boundary is unclear, Gateway and Login easily get mixed into one layer
```

### theseed's Trade-offs

```
theseed chooses to write Gateway and Login Service separately,
and to establish the Challenge ops boundary explicitly,
avoiding shrinking the login service into "a single access port".
```

### Why There Cannot Be Only a Gateway Left

```
If Gateway and Login are merged into "a single access service",
three kinds of confusion quickly appear:
  1. connection access and the challenge state machine become tightly bound
  2. TLS / protocol adaptation and risk control / ban / pending request get mixed together
  3. login-plane ops capability degrades into "querying the connection count"
```

---

## 0. Design Boundary

This document covers:

```
  - external access path
  - responsibility split between Gateway and Login Service
  - challenge / pending / ban / session handoff
  - login-plane ops boundary
```

This document does not cover:

```
  - in-game Entity runtime
  - MessageBus control plane
  - Watcher-style unified control entry point
  - OTel backend and visualization platform
```

These topics are covered respectively in:

```
02-message-bus-and-cross-realm
04-ops-control-plane
05-telemetry-and-debug
../3-cluster-and-availability/03-cluster-lifecycle
```

---

## 1. Why Gateway and Login Belong in the Same Layer

The old structure splitting `Gateway` and `Login Service` into two documents is fine,
but placed in the old directory it invites misjudgment:

```
Gateway = external access
Login Service = some functional sub-page
```

The more accurate understanding is:

```
Gateway + Login Service
  together form the Access Plane
```

Wherein:

```
Gateway
  responsible for connection access, protocol adaptation, TLS, coarse-grained rate limiting

Login Service
  responsible for challenge, pending request, risk control, ban, statusCheck, login session handoff
```

This is also the system boundary that BigWorld's `LoginApp` truly embodies.

So the more accurate classification is:

```
Gateway
  = access proxy layer

Login Service
  = login state machine + risk control + session issuance layer
```

---

## 2. Overall Architecture

```text
Client
  ├─ TCP / WebSocket / HTTP
  ▼
Gateway
  ├─ TLS termination
  ├─ protocol adapter
  ├─ coarse rate limit
  └─ route to login
  ▼
Login Service
  ├─ challenge registry
  ├─ request tracker
  ├─ rate-limit / ban store
  ├─ session issuer
  └─ ops hooks
  ▼
Session / Router handoff
  ▼
Base / Game entry
```

---

## 3. Gateway Responsibilities

Gateway is responsible for:

```
  - external port listening
  - TLS termination
  - protocol unpacking and connection management
  - coarse-grained IP / connection rate limiting
  - handing login traffic over to the Login Service
```

Gateway is not responsible for:

```
  - challenge lifecycle management
  - pending login deduplication
  - ban cleanup policy
  - login statistics dashboard
```

### 3.1 Interfaces

```cpp
class IGateway {
public:
    virtual void start(const GatewayConfig& config) = 0;
    virtual void stop() = 0;
    virtual size_t activeConnections() const = 0;
    virtual void kickConnection(ConnectionId id, const std::string& reason) = 0;
    virtual GatewayStats getStats() const = 0;
};
```

---

## 4. Login Service Responsibilities

Login Service is responsible for:

```
  - generate and verify challenge
  - maintain pending login request
  - count challenge success / failure / retry
  - temporary ban / rate limit / request dedupe
  - session issuance after a successful login
  - statusCheck / clearTemporaryBans / draining
```

### 4.1 Core Interfaces

```cpp
class ILoginService {
public:
    virtual ~ILoginService() = default;

    virtual Future<LoginReply> beginLogin(const LoginRequest& request) = 0;
    virtual Future<LoginReply> verifyChallenge(
        const ChallengeVerificationRequest& request) = 0;

    virtual Future<void> clearTemporaryBans() = 0;
    virtual LoginStatsSnapshot stats() const = 0;
};
```

### 4.2 Challenge Abstraction

```cpp
class ILoginChallenge {
public:
    virtual ~ILoginChallenge() = default;

    virtual std::string type() const = 0;
    virtual Challenge generate(const LoginRequestMeta& meta) = 0;
    virtual VerifyResult verify(const Challenge& challenge,
                                const ChallengeResponse& response,
                                const LoginRequestMeta& meta) = 0;
};
```

---

## 5. Login Request State Machine

```text
Idle
  → Pending
  → ChallengeIssued
  → ChallengeVerified
  → SessionIssued
  → Complete

失败路径：
  Pending → Rejected
  ChallengeIssued → ChallengeFailed
  ChallengeIssued → Expired
```

Key constraints:

```
1. repeated login requests must not issue challenges indefinitely
2. challenge reissue must continue on the original request
3. challenge failure must enter statistics and risk control
4. session issuance and request completion must be auditable
```

---

## 6. Ops Boundary

BigWorld's login plane is not "a few commands tacked onto the login code";
it is a small ops subsystem.

The theseed documentation now explicitly splits it into three categories of capability:

### 6.1 Inspect

```
  - pending login count
  - challenge type
  - successes / failures / rate limited
  - ban store summary
  - current draining state
```

### 6.2 Command

```
  - statusCheck
  - clearTemporaryBans
  - setChallengeType
  - setDraining
  - controlledShutdown
```

### 6.3 Guardrail

```
  - protocol fields must not be changed online
  - session authority must not be cleared directly bypassing the audit
  - challenge policy switching must verify that the implementation is registered
```

The unified command entry point and permission model are not expanded in this document; see:

`04-ops-control-plane`

---

## 7. Session Handoff and Routing

After a successful login, the Access Plane is only responsible for completing the handoff; it does not take over in-game authority.

```text
Login Service
  → issue session / token
  → choose entry route
  → hand off to gateway router / game gateway
  → runtime authority moved to Base / game session side
```

This point must stay consistent with the old-style LoginApp approach of KBEngine / BigWorld:

```
The login service is not a game-logic host.
```

---

## 8. Configuration Boundary

```yaml
gateway:
  listeners:
    - protocol: tcp
      port: 20000
    - protocol: websocket
      port: 20001

  tls:
    cert_file: /etc/theseed/certs/server.pem
    key_file: /etc/theseed/certs/server.key

login_service:
  challenge:
    default_type: hash_pow
    timeout_ms: 10000
    max_retries: 2

  rate_limit:
    per_ip_per_minute: 30
    pending_per_ip: 4

  bans:
    temporary_ttl_sec: 1800
```

---

## 9. Phased Boundaries

```text
MVP:
  - single challenge type
  - basic pending management
  - IP rate limiting
  - statusCheck / clearTemporaryBans

Phase 2:
  - challenge factory registry
  - richer statistics
  - account / device dimension risk control

Phase 3:
  - multiple challenge policies
  - integration with the unified Ops platform
  - finer-grained access orchestration
```

---

## 10. Comparison with BigWorld / KBEngine / theseed

| Dimension | BigWorld | KBEngine | theseed |
|------|------|------|------|
| Login Challenge | built in, multiple factories | basically none | standalone Login Service |
| statusCheck | yes | weaker | explicitly retained |
| ban cleanup command | yes | weaker | explicitly retained |
| Gateway | simplified access plane | simplified TCP forwarding | standalone Gateway |
| login ops plane | yes | weaker | design boundary established |

---

## 11. One-Sentence Judgment

The focus of this document is not "how login handshakes", but rather:

```
theseed has already elevated the BigWorld-style login service system boundary
from mere access details to an independent capability surface of the Access Plane.
```

---

## 12. LoginApp — Implementation Baseline Referencing BigWorld

> This section first returns to BigWorld's implementation approach as the implementation baseline for the current phase.
> LoginApp is first implemented as the core login-plane service, covering authentication, realm list, character summaries, risk control, and session handoff,
> without first introducing the abstraction refactor of an assemblable Pipeline.

### 12.1 LoginApp Responsibilities

LoginApp references BigWorld's login-plane implementation; its responsibilities include:

```text
  - authentication
  - realm list query
  - character summary return
  - pending request maintenance
  - temporary ban / risk control
  - statusCheck
  - session handoff
```

LoginApp does not undertake the in-game Entity runtime, does not undertake the message bus control plane, and does not undertake the unified ops platform.

### 12.2 Login Flow

```text
Client
  → login(account, credentials)
  → LoginApp verifies the account
  → returns realm list + per-realm character summary + realm ip:port

Client selects a realm
  → connect realm.ip:realm.port
  → loginGateway(account, token, realmId)
  → realm verifies the session
  → create Account Entity
  → reqAvatarList / selectAvatarGame
```

In single-realm mode the realm selection can be skipped, but the default implementation still handles it as a realm entry.

### 12.3 Implementation Points Compared with BigWorld

```text
BigWorld-style key points:
  - LoginApp is an independent login-plane subsystem
  - login, risk control, ban, statusCheck, session handoff live within the same boundary
  - realm information and character summaries are returned by the login plane
  - the client ultimately connects directly to the realm entry
```

### 12.4 Current Implementation Baseline

```cpp
struct RealmInfo {
    std::string realmId;
    std::string name;
    std::string status;
    std::string host;
    uint16_t port;
    std::vector<CharacterSummary> characters;
};

class ILoginApp {
public:
    virtual ~ILoginApp() = default;
    virtual LoginReply login(const LoginRequest& request) = 0;
    virtual RealmList queryRealms(const AccountId& accountId) = 0;
    virtual LoginReply selectRealm(const AccountId& accountId,
                                   const std::string& realmId) = 0;
    virtual void clearTemporaryBans() = 0;
    virtual LoginStatsSnapshot stats() const = 0;
};
```

### 12.5 Configuration Baseline

```yaml
login_app:
  endpoint: login.example.com
  auth:
    type: password
  realm_source:
    type: static_list
  realms:
    - id: "realm_east"
      name: "华东一区"
      host: "10.0.1.1"
      port: 20000
    - id: "realm_south"
      name: "华南一区"
      host: "10.0.2.1"
      port: 20000
  bans:
    temporary_ttl_sec: 1800
  challenge:
    enabled: false
```

### 12.6 Plans Pending Discussion

The following content is kept for discussion for now and does not enter the current implementation baseline:

```text
  - assemblable Login Pipeline
  - standalone Gateway process
  - login endpoint discovery / DNS polling / LB
  - policy-driven realm entry (Static / Registry / Fixed)
  - Challenge state machine enhancement
  - finer-grained risk control policy
```

### 12.7 Relationship Between the Design Document and the Implementation Baseline

Chapters 1-11 of this document describe the complete system boundary of the theseed Access Plane.
Chapter 12 currently adopts the BigWorld-style implementation baseline first, and later decides whether to introduce a more abstract pipelined approach based on validation results.
