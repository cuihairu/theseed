# Ops Control Plane — Ops Control Plane and Distributed Observation

> English | **中文**

> Origin: BigWorld `Watcher / ForwardingWatcher / EntityProfiler / challenge watcher / controlled shutdown`.
> Reference implementation: the lightweight management path of KBEngine's `Watcher / WebConsole`.
> This layer is neither OTel nor the Runtime Data Plane; it is an independent plane for online ops and control.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
BigWorld's Watcher system is not simple monitoring:
  - distributed query
  - online commands
  - hot configuration change
  - status check
  - controlled shutdown entry point
```

### How KBEngine Implements It

```
KBEngine also has Watcher / WebConsole-class capabilities,
but its ops control plane is not as systematic as BigWorld's.
```

### Pros and Cons

```
BigWorld's advantages:
  - complete ops control plane
  - strong remote diagnosis capability

KBEngine's advantages:
  - simple
  - low implementation cost

Shared drawbacks:
  - once the control plane and the observation plane are written together, the boundary quickly gets out of control
```

### theseed's Trade-offs

```
theseed separates out the Ops Control Plane,
clarifying that it is responsible for Inspect + Command + Config + Audit,
and not letting it degrade into "yet another monitoring dashboard".
```

### Why Liveness Probes and State Exposure Cannot Both Be Pushed into Telemetry

```
The core of Telemetry is collection and diagnostic output.
The core of Ops is:
  - whether it can currently accept traffic
  - whether it can currently execute commands
  - whether the current state can serve as input for ops decisions

So although /health and /status/summary "look like observation interfaces",
they essentially belong to the control plane's public boundary.
```

---

## 0. Design Boundary

```
Ops Control Plane is responsible for:
  - remote query of runtime state
  - remote execution of controlled commands
  - online adjustment of a small amount of safe configuration
  - auditing ops actions

Ops Control Plane is not responsible for:
  - EntityCall main path
  - ghost / witness / migration data transfer
  - metrics / traces / logs storage backend
  - game business cross-realm messages
```

In other words:

```
OTel is responsible for Telemetry
Ops Control Plane is responsible for Inspect + Command + Config + Audit
```

---

## 1. Why OTel Alone Is Not Enough

BigWorld's `Watcher` is not a simple metrics system.

It simultaneously carries:

```
1. Distributed state tree query
   - process, entity, Cell, Space, DB, login challenge configuration

2. Online command entry point
   - statusCheck
   - clear IP bans
   - retire / drain / controlled shutdown

3. Online hot configuration change
   - challengeType
   - sampling, thresholds, rate limiting

4. Load diagnosis
   - entity profiler
   - entity type profiler
```

If all of these were pushed into the OTel documentation, "observation" and "control" would be mixed into one layer.

A stricter rule can be written as:

```
Show flame graphs, traces, metrics to humans
  → Telemetry

Show ready / live / startup to the platform
  → Ops Control Plane

Issue set draining / retire / shutdown to operators
  → Ops Control Plane
```

---

## 2. Capability Scope

### 2.1 Inspect

```
Process level:
  - role, version, startup time, health state
  - current load, queue length, routing summary

Runtime level:
  - entity count
  - witness / ghost overview
  - migration backlog
  - archiver / backup / session state

Business level:
  - login challenge configuration
  - ban state
  - read-only inspection of a specified entity
```

### 2.2 Health Probes and State Exposure

Liveness probing must be split into three categories:

```
startup probe
  - whether the process has finished starting
  - whether script / defs / route / listener have finished initialization

liveness probe
  - whether the tick main loop is still advancing
  - whether key threads are stuck
  - whether it has entered fatal self-deadlock

readiness probe
  - whether it currently accepts new traffic / new logins / new entity migration in
  - whether it is in draining / retiring / admitted overload
  - whether dependencies satisfy the minimum serviceable condition
```

The runtime state exposed externally should also be tiered:

```
Public summary:
  - role / version / uptime / process state
  - ready / live / startup state
  - draining / overload / route-ready summary

Controlled details:
  - complete state tree
  - entity / session / backup / migration details
  - profiler sampling and diagnostic artifacts
```

Requirements:

```
1. liveness-probing interfaces may be called anonymously or from the intranet by the platform
2. the complete state tree must not be exposed anonymously
3. readiness failure does not mean the process must be restarted
4. only liveness failure means it should be brought up by the supervisor
```

### 2.3 Command

```
Only idempotent or controlled commands are allowed:
  - kick session
  - set draining
  - retire process
  - controlled shutdown
  - clear transient bans
  - reload sampled config
```

### 2.4 Config

```
Configuration allowed to be changed online must satisfy:
  - clear scope of impact
  - an audit record
  - a rollback entry point
  - no breaking of protocol and persistence compatibility
```

---

## 3. Architecture

```
Ops Console / CLI
  → AuthN / AuthZ
  → Ops Control Gateway
  → Target Resolver
  → Per-Process Ops Agent
  → Runtime Adapter
```

Responsibilities of each layer:

```
Ops Console
  - human or automated entry point

Ops Control Gateway
  - permission verification
  - request audit
  - rate limiting and timeout

Target Resolver
  - resolve the target by realm / role / process / entity

Ops Agent
  - interface with the in-process state tree and command registry
```

Typical entry points for liveness probing and state exposure:

```
/health/startup
/health/live
/health/ready
/status/summary
```

Wherein:

```
/health/*
  - facing the scheduler / supervisor / ingress

/status/summary
  - facing the ops dashboard / CLI / automation platform
```

---

## 4. Unified Object Model

### 4.1 State Tree

```cpp
// ops/IOpsNode.h

class IOpsNode {
public:
    virtual ~IOpsNode() = default;

    virtual std::string name() const = 0;
    virtual std::vector<std::string> children() const = 0;
    virtual JsonValue read(const std::string& path) const = 0;
};
```

### 4.2 Command Interface

```cpp
// ops/IOpsCommand.h

struct OpsCommandContext {
    std::string operatorId;
    std::string requestId;
    std::string sourceIp;
};

class IOpsCommand {
public:
    virtual ~IOpsCommand() = default;

    virtual std::string name() const = 0;
    virtual OpsResult execute(const JsonValue& args,
                              const OpsCommandContext& ctx) = 0;
};
```

### 4.3 Agent Interface

```cpp
// ops/IOpsEndpoint.h

class IOpsEndpoint {
public:
    virtual ~IOpsEndpoint() = default;

    virtual Future<JsonValue> inspect(const std::string& path) = 0;
    virtual Future<OpsResult> call(const std::string& command,
                                   const JsonValue& args,
                                   const OpsCommandContext& ctx) = 0;
};
```

---

## 5. Relationship with Runtime / MessageBus / OTel

| Plane | Role | Typical Traffic |
|------|------|---------|
| Runtime Data Plane | entity main path | EntityCall, ghost sync, migration snapshots |
| Control Plane | process registration, routing, lifecycle coordination | online/offline, redeploy, drain |
| Ops Control Plane | observation, commands, online adjustment | inspect, retire, statusCheck |
| OTel Telemetry | traces / metrics / logs | span, metric, structured log |

Requirements:

```
1. Ops Control Plane does not carry high-frequency entity data streams
2. OTel is not responsible for executing control commands
3. Runtime failure should not drag down the audit and command permission boundary
4. liveness probing only exposes summary state and does not replace complete diagnosis
```

---

## 6. Security Model

### 6.1 Permission Tiers

```
ReadOnly
  - inspect process / metrics summary / config snapshot

Operator
  - kick session / set draining / clear temporary bans

Admin
  - controlled shutdown / retire process / apply runtime config
```

### 6.2 Audit Requirements

All write operations must record:

```
  - operatorId
  - requestId
  - target
  - command
  - args digest
  - result
  - timestamp
```

### 6.3 Online Configuration Change Restrictions

It is forbidden to modify the following online through the Ops Control Plane:

```
  - protocol definitions
  - persistence schema
  - entity property flags
  - migration semantics
```

---

## 7. Where Profiling and Watcher Belong

BigWorld's entity-level / type-level profiler should not be treated merely as metrics.

theseed recommends splitting it this way:

```
OTel:
  - metric time series
  - traces and logs

Ops Control Plane:
  - query the current profile by entity / entity type / process
  - trigger a sampling on demand
  - query retirement, drain, migration backlog
```

The ownership of diagnostic artifacts such as flame graphs is:

```
sampling/export semantics
  - see 05-telemetry-and-debug

trigger/download/authentication entry point
  - see this document, Ops Control Plane
```

This preserves cloud-native Telemetry while not losing the BigWorld-style ops control plane.

---

## 8. Phased Boundaries

```
MVP:
  - read-only inspect
  - a small amount of controlled commands: statusCheck / kick / draining
  - operation audit

Phase 2:
  - forwarding aggregation
  - profiler / entity-type diagnostics
  - more complete login and session ops commands

Phase 3:
  - cross-realm ops proxy
  - integration with release, hot update, and data ops platforms
```

Implementation mapping (theseed repository, 2026-09-26 measured definition):

```
MVP landed:
  - read-only inspect: machine.snapshot / machine.audit / process enumeration /
    profile listing and download (agent side + center side), ≥ReadOnly;
  - a small amount of controlled commands: machine.kick-session (≙ kick, revoking the SessionStore
    session), machine.set-draining (≙ draining, snapshot and reporting exposed from the same source)
    ——≥Operator; machine.shutdown (≙ §6.1 controlled shutdown,
    graceful shutdown at the end of the tick after the response goes out) ——≥Admin;
  - operation audit: all attempts (including rejections) go into the daemon's local ring buffer + central aggregation;
    §6.2 all seven fields aligned —— requestId is carried by the RuntimeInvocation frame
    (u64, 0 = not carried), transparently passed per request into the audit entry and the machine.audit JSON
    request_id field.

Not landed within MVP (degradation stated honestly):
  - statusCheck is not built separately: machine.snapshot is the read-only liveness-probing semantics;
  - clear temporary bans: the repository has no temporary ban storage, so the missing precondition is not fabricated
    (§6.1 tier is kept on record; see the boundary in todo.md).

Phase 2 landed slices (session ops command surface, 2026-09-26):
  - machine.list-sessions (≥ReadOnly): enumerates via the SessionStore secondary index
    live sessions; the inline token only echoes a length fingerprint (zero plaintext leakage); account/realm/user id
    are available for ops selection;
  - machine.kick-sessions (≥Operator, ≙ batch kick): scope selector
    all | account=<id> | realm=<id> (the fingerprint is irreversible, no batching by token list),
    revokes one by one, returning revoked/missing in separate columns to the caller; partial failures still enter the audit;
  - machine.extend-sessions (≥Operator, ≙ session renewal policy): the same scope
    selector; refreshes the selected sessions to the policy TTL (Config.sessionRenewalTtl,
    the policy value is not specified ad hoc by the caller; extending the policy requires a config review —— the same
    discipline as the §6.3 whitelist; 0 = entry disabled, treated as an unknown method); extended/missing in separate columns;
  - kick-linked notification (≙ kick linkage): after a single or batch kick revocation succeeds, pushes
    machine.session.revoked to Config.sessionNotifyComponent (payload
    {account, realm, session=fingerprint, reason=operator.kick |
    operator.kick.batch}); best-effort —— no retry/no acknowledgement; if the target is not registered
    the alert is dropped (the revocation fact is authoritative from storage; after reconnection it can be reconciled via list-sessions);
    the token plaintext does not leave the process.
  - production wiring of the notification (2026-09-27, feedback loop): LoginApp connects outbound to the daemon and sends one
    machine.snapshot registration probe to self-report its identity (the daemon hub reuses attachServerTransport's
    first-request self-report registration, the same mechanism as DBApp —— no new registration protocol is added); the tick drains inbound
    RuntimeInvocation to the dispatch surface: machine.session.revoked decodes the payload and calls
    handleSessionRevoked (closes matching active login connections by account+realm;
    the binding table is removed in sync with the disconnect sweep); probe replies (snapshot.ok / machine.error)
    record the linkage state; other methods are logged and counted, not silently dropped. Payload parsing strictly aligns with the control-side
    escapeJsonString escape set; escapes outside the set are rejected as malformed.
  - correction of the realm definition for session rows: login writes only the base session (realm empty); on realm selection, realm
    is written back to the same row (load first, then rewrite; expired rows are not resurrected). Previously the two sides' definitions were inconsistent ——
    the notification's realm came from the storage row, while the binding's realm came from the local selection; the linkage would miss
    clients that were truly "realm-selected after login"; meanwhile list-sessions' realm field
    was always empty. After the fix, list-sessions' realm and the kick notification are usable from the same source.
  - runtime resilience of the notification leg (2026-09-27, feedback loop): LoginApp supervises the outbound leg within the tick ——
    liveness is based on the transport's live state (IRuntimeTransport::isConnected; a peer EOF
    is turned false by the socket pump; the TCP layer's 30s heartbeat makes a half-open with no EOF also converge via read/write errors;
    the in-memory implementation defaults to true) + any inbound proof from the daemon direction (probe reply or push, either is fine;
    registration is independent at the transport layer and the policy gate). A broken link or a probe reply exceeding the window (machineProbeAckTimeout
    —— a connect return does not count; a Linux non-blocking connect is always EINPROGRESS) immediately removes the
    dead peer, reconnects with exponential backoff (starting at machineReconnectBaseDelay, doubling up to the cap
    machineReconnectMaxDelay, reset on recovery), and resends the machine.snapshot registration
    probe; the daemon-side hub re-registers its subscription via self-report on the new connection —— after a daemon restart,
    LoginApp needs no manual restart, and revocation notifications continue to be delivered (provable over real TCP end to end).
    During disconnection it is still best-effort: only logWarn + count
    (login_machine_link_{down,up}_count), without blocking the login plane.
  - runtime resilience of the db leg (2026-09-27, feedback loop): the DBApp outbound link of the login data plane
    gains the same supervision —— a broken link (transport live state / connection probe reply exceeding the window
    dbProbeAckTimeout) removes the dead peer, reconnects with exponential backoff
    (starting at dbReconnectBaseDelay, doubling up to the cap dbReconnectMaxDelay, reset
    on recovery), and resends the connection probe; the probe uses db.listTypes (read-only, always answers; the reply method
    does not overlap with the login request's match string —— a late probe reply will not be mistakenly
    taken as a login reply by an in-flight dbRequest; DBApp-side registration for a new connection is still done via the first request's sourceComponent
    self-report, requiring no new protocol). The db leg is pull-based: any DBApp reply (including that of a real login)
    proves liveness. During the disconnection window, the absent peer makes dbRequest immediately NotConnected,
    reusing the existing "database unavailable" timeout degradation semantics, without blocking the tick. Counts
    login_db_link_{down,up}_count; after a DBApp restart, LoginApp needs no manual restart, and
    login queries recover (provable over real TCP end to end).

Still open in Phase 2:
  - forwarding aggregation / entity-type diagnostics;
  - the remaining parts of more complete login and session ops commands (clear temporary bans is still not done because
    the ban storage precondition is missing, following the existing boundary);
  - notifications missed during disconnection are not resent (no reconciliation protocol) —— reconnection restores the subscription, not
    historical facts; the revocation is authoritative from storage, and ops reconciles via list-sessions (the db leg transport
    has gained the same runtime resilience, see above; login requests within the disconnection window fail directly per the existing definition,
    without queuing or replaying).
```

---

## 9. Comparison with BigWorld / KBEngine

| Dimension | BigWorld | KBEngine | theseed |
|------|---------|---------|---------|
| Distributed observation | Watcher + ForwardingWatcher | weaker Watcher | standalone Ops Control Plane |
| Online commands | rich built-in | weaker | command registration + audit |
| Entity-level profile | yes | weaker | OTel + Ops query layering |
| Online configuration change | partial support | partial support | explicit permission and rollback boundaries |
