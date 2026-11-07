# theseed Architecture Lineage: From BigWorld and KBEngine to a Modern Implementation

> **English** | 中文

> This document answers three questions: what theseed borrowed from BigWorld and KBEngine respectively, what it changed, and why.
> Detailed arguments per system area are in the eight-layer design documents under `docs/design/` (0-foundation through 8-reference),
> and item-by-item source tracing is in `docs/design/8-reference/source-attribution.md`.

## 1. Positioning

theseed is a distributed game server engine for modern MMOs:

- **BigWorld (14.4.1) sets the upper bound of system boundaries** — the server-side system areas (Space/AOI/Witness/Ghost/
  HA/data ops/login/control plane) are audited against it as the origin boundary;
- **KBEngine helps validate the minimal runtime loop** — its lightweight implementation of tick/Entity/AOI/EntityCall
  is an important reference, but its simplified capabilities are not treated as an upper bound;
- **theseed is responsible for modernization and engineering implementation** — C++23, monorepo + vcpkg, separation of the three planes,
  and test and coverage red lines (see `docs/design/0-foundation/04-modern-mmo-engine-positioning.md`).

theseed is neither a rewrite of KBEngine nor an item-by-item port of BigWorld: the old engines were "correct in capability but mixed across layers",
whereas theseed explicitly separates the entity main path from ops commands, metrics display from control commands, profiler sampling from scheduling decisions,
and the storage main path from offline data ops, each assigned to its own layer.

## 2. Lineage Overview Table

| Layer | Inherited from BigWorld | Referenced from KBEngine | theseed's changes |
| --- | --- | --- | --- |
| Runtime model (tick/Entity) | Base/Cell dual bodies, single-threaded tick main loop | `EventDispatcher::processUntilBreak`-style segmented tick, lightweight entity implementation | PropertyBlock contiguous memory replaces Python dict attributes; TimerWheel selection; C++23 throughout |
| Replication and space | AOI cross-linked list/RangeTrigger, Witness/RealEntity/EntityCache/Ghost/Haunt, Mailbox(EntityCall), BSP load balancing | EntityCall unidirectional simplification, migration routing window (GhostManager) | Runtime Data Plane does not go through the message bus (own judgment); migration routing window semantics tightened |
| Cluster and availability | BackupHash/BackupHashChain/BackupSender hot-standby chain of responsibility | (relatively weak, reference only) | BackupTopologyCoordinator prototype + LoadProfiler load feedback |
| Data and ops | Property-level storage mapping (15+ kinds of PropertyMapping), Archiver/SecondaryDB/consolidate_dbs toolchain, Server Merge | MySQL main path, 5 kinds of simplified TABLE_ITEM mapping | file/MySQL/PostgreSQL three backends behind one interface (`IEntityStore`/`IAccountStore`); BigWorld only supports MySQL+XML |
| Access and control plane | LoginApp login plane (Challenge/statusCheck/ban cleanup), bwmachined, Watcher/message_logger/profiler | Lightweight access path, machine process management | Redis session/rate-limiting stack, read-only Ops HTTP (/metrics /health /inspect), OTel-style Tracing — all newly added |
| World and game framework | Physics2, Recast/Detour navigation, 30+ kinds of Controller, Compiled Space/world streaming | Concise interface for 4 kinds of Controller | Take BigWorld's capability boundary + KBEngine's implementation approach; currently implements MoveToPoint/MoveToEntity |
| Scripting and client | L1-L4 tiered hot update concept, SDK digest verification | Hybrid client SDK generation chain (referenced but decoupling the generation chain from runtime binding) | Restricted L1/L2 hot update prototype; modern codegen (C# Emitter); Merkle diff replaces a single digest |

## 3. Key Changes and Rationale

### 3.1 Attribute layout: PropertyBlock contiguous memory (replacing KBEngine's Python dict)

KBEngine stores attributes in a Python dict, so every read and write goes through the Python API, resulting in poor performance and cache locality
(`docs/design/1-runtime-model/02-entity-system.md`). theseed arranges attributes into a contiguous `PropertyBlock` according to the entity definition,
using a DirtyMask for dirty marking and batch replication per tick.
This is a typical change where "KBEngine validated the loop, and theseed replaced the implementation form".

### 3.2 Making reliability semantics explicit

BigWorld's Channel/Bundle has two kinds of semantics, Ordered Reliable and Unordered Lossy, but they are coupled to the transport implementation.
theseed models them explicitly on `IRuntimeTransport`/`NetworkTransport`:
EntityCall/control messages use Ordered Reliable, while volatile/AOI broadcasts use Unordered Lossy,
with a `TransportStatsCollector` collecting Channel/Bundle state.

### 3.3 Separation of the three planes (theseed's own judgment)

- **Runtime Data Plane**: entity tick, EntityCall, property replication — single-threaded tick, does not go through the message bus;
- **Control Plane**: process orchestration (MachineAgent/ProcessSupervisor), Ops read-only plane — separate HTTP/TCP ports;
- **Cross-Realm Async Plane**: cross-server async (message bus, Redis) — isolated from the runtime data plane.

KBEngine mixes these into a single process model; BigWorld is complete in capability but its observation/control/scheduling permeate each other.
The motivation for layering is fault-domain isolation and testability.

### 3.4 Storage abstraction and multiple backends

The `IEntityStore`/`IAccountStore` interfaces plus three implementations — FileEntityStore/MySQLEntityStore/
PostgreSQLEntityStore — with automatic fallback to the file backend when the client library is missing at build time
(`THESEED_HAS_MYSQL`/`THESEED_HAS_POSTGRESQL` conditional compilation).
BigWorld only supports MySQL (+XML); KBEngine takes MySQL as its main path. The storage model is unified as
"one `tbl_<Type>` table per type + native scalar columns + VECTOR3 expansion + variable-length JSON/BYTEA".

### 3.5 Engineering baseline

monorepo + vcpkg manifest + CMake presets (five trees: gcc/clang/msvc/coverage/sanitize);
C++23; `-Wall -Wextra -Wpedantic -Werror` (`THESEED_WARNINGS_AS_ERRORS=ON` enabled by default);
each module has unit/integration tests, with 100% coverage across the three metrics of line/branch/function (exemptions require experimental justification, see
`docs/design/8-reference/coverage-report.md`).

## 4. Avoiding BigWorld's Legacy Issues

| BigWorld's pitfall | theseed's countermeasure | Landing point |
| --- | --- | --- |
| Heavy coupling between the core and scripting/platform | Pure C++23 core library layering (foundation→runtime→core/db/login/...), with the scripting layer going only through restricted interfaces | `src/` layering and dependency direction |
| Platform lock-in (Windows server toolchain, custom builds) | CMake ≥3.28 + vcpkg manifest, a three-compiler matrix of gcc/clang/msvc + coverage/sanitize trees | `CMakePresets.json` |
| Missing tests, regression done manually | 120 tests passing green as the commit gate; a 100% red line across the three coverage metrics; a test written but not registered is caught by `-Wunused-function` | `tests/`, CI, `-Werror` |
| Observation/control/data planes permeating each other | Separation of the three planes; the Ops read-only plane on a separate HTTP port; control commands separated from metrics display | `src/ops/`, architecture baseline documents |
| The database layer only supports MySQL | The `IEntityStore` abstraction + three backends, with the SQL integration section gated by environment variables for reproducible verification | `src/db/` |

## 5. Current Implementation Status (as of 2026-09)

- **Phase A (complete)**: TickScheduler, Entity/Base-Cell dual bodies, PropertyBlock,
  Base↔Cell EntityCall, SingleCell Space, Witness synchronization, FileEntityStore, minimal login.
- **Phase B (in progress)**: cross-linked list AOI ✅, Controller (MoveToPoint/MoveToEntity) ✅,
  MySQL/PostgreSQL persistence backends ✅ (verified against real instances), Redis session/rate limiting ✅,
  C# code generation ✅, Metrics/Logger/Tracing ✅, read-only Ops inspect ✅,
  Channel/Bundle state statistics ✅.
- **Phase C (started, pending end-to-end integration)**: restricted hot update (L1/L2), migration routing window,
  BackupTopologyCoordinator, LoadProfiler.
- Process targets: loginapp / realmapp / baseapp / cellapp / dbapp / machine (`apps/`).
- Code map: `src/foundation` (infrastructure) / `src/runtime` (runtime core) /
  `src/core` (application skeleton and EntityDef) / `src/db` / `src/login` / `src/realm` /
  `src/ops` / `src/control` / `src/scripting`.

## 6. Quality Red Lines (standards enforced in this repo)

1. Modern C++: RAII, smart pointers, no bare `new`/`delete`, const correctness;
2. Unified error handling and logging (return bool/optional within boundaries, logging goes through `foundation/Logger`);
3. Interfaces have comments and clear boundaries (pure interface classes accept a coverage exemption for a trivial virtual destructor);
4. No magic numbers and no duplicated code;
5. Zero compile warnings: `-Wall -Wextra -Wpedantic` + `-Werror` (can be
   temporarily disabled via `THESEED_WARNINGS_AS_ERRORS=OFF`, enabled by default);
6. Each module has unit tests that keep passing; a full green ctest before commit is a hard gate;
   a coverage miss must either be covered by adding tests or carry an exemption marker with experimental justification.

## 7. Abstract Source Layers

The abstract layers below are responsibility boundaries found in the sources; the concrete directory appears only as evidence.

| Layer | Responsibility | Representative file |
| --- | --- | --- |
| LAY-001 Process Assembly | Parse process input, assemble transport/store/app, and drive init → tick → stop. | `apps/baseapp/main.cpp:117` |
| LAY-002 Foundation | Own cross-cutting serialization, routing, timers, logs, metrics, tracing, Redis-shaped state, and rate limits. | `src/foundation/include/theseed/foundation/ChannelRouter.h:13` |
| LAY-003 Runtime Transport & Scheduling | Frame and route `RuntimeInvocation`, manage TCP/in-memory peers, and execute six tick phases. | `src/runtime/include/theseed/runtime/TickScheduler.h:16` |
| LAY-004 Entity & Space Runtime | Own entity property/AOI/witness/ghost/migration state plus load and backup topology. | `src/runtime/include/theseed/runtime/CellRuntime.h:26` |
| LAY-005 Application Services | Compose protocols, sessions, defs, entity runtime, storage, and observability into Base/Cell/Login/Realm/DB use cases. | `src/core/include/theseed/core/BaseApp.h:26` |
| LAY-006 Persistence | Normalize file, MySQL, PostgreSQL, and remote-DB access behind `IEntityStore`/`IAccountStore`. | `src/db/include/theseed/db/RemoteEntityStore.h:17` |
| LAY-007 Observability & Control | Expose read-only HTTP inspection, host RPC, aggregation, policy/audit, and OTLP export away from the game tick. | `src/ops/include/theseed/ops/OpsServer.h:24` |
| LAY-008 Tooling & Verification | Use XML definitions and client protocols externally for code generation and protocol load tests. | `tools/codegen/main.cpp:3` |

### 7.1 LAY-001: Process Assembly

- **Implementation:** `apps/baseapp/main.cpp`, `apps/cellapp/main.cpp`, `apps/dbapp/main.cpp`, `apps/loginapp/main.cpp`, `apps/machine/main.cpp`, and `apps/realmapp/main.cpp`; shared signal/number parsing is in `apps/AppMainUtil.h:23`.
- **Configuration schema:** main-local structs such as `PeerConfig` and `DbConfig` (`apps/baseapp/main.cpp:18`) fill each service's config struct; no external config-file schema exists for these processes.
- **Protocol files:** none; assembly only invokes service APIs.
- **User-facing copy:** startup/stderr messages in each `main.cpp`; shared parser diagnostics in `apps/AppMainUtil.h:42`.
- **Lifecycle:** install signals → construct config/dependencies → `init()`/`attach()` → signal-driven `tick()` loop → `detach()`/`stop()` (representative: `apps/baseapp/main.cpp:117`, `apps/baseapp/main.cpp:142`).

### 7.2 LAY-002: Foundation

- **Implementation:** `src/foundation/*.cpp`, including `ChannelRouter`, `RedisProvider`, `SessionStore`, `RateLimiter`, `TimerWheel`, `MetricsRegistry`, `Logger`, and `Tracing`.
- **Configuration schema:** `Channel::Watermark` (`src/foundation/include/theseed/foundation/Channel.h:20`), `RateLimiter::Config` (`src/foundation/include/theseed/foundation/RateLimiter.h:23`), and type-level state model contracts in `RedisProvider.h`.
- **Protocol files:** `MessageHeader` and `Bundle` define the reusable message framing primitives; `src/foundation/include/theseed/foundation/Tracing.h:73` defines W3C-style carrier injection rather than a standalone network protocol.
- **User-facing copy:** log event names/attributes produced by `Logger` and metric descriptions held by `MetricsRegistry`.
- **Lifecycle:** objects are constructed by owners; `TimerWheel` advances through owner ticks, and `MetricsRegistry` is a process-wide singleton (`src/foundation/include/theseed/foundation/Metrics.h:75`).

### 7.3 LAY-003: Runtime Transport & Scheduling

- **Implementation:** `src/runtime/NetworkTransport.cpp`, `TcpConnection.cpp`, `TcpListener.cpp`, `TransportHub.cpp`, `InvocationCodec.cpp`, `RuntimeLoop.cpp`, and `TickScheduler.cpp`.
- **Configuration schema:** `NetworkTransport::Config` (`src/runtime/include/theseed/runtime/NetworkTransport.h:31`), `NetworkNode::Config`, `RuntimeLoop` scheduler settings, and `TickScheduler` interval construction (`src/runtime/include/theseed/runtime/TickScheduler.h:52`).
- **Protocol files:** `RuntimeInvocation` and `InvocationCodec` are the dedicated wire contract (`src/runtime/include/theseed/runtime/InvocationCodec.h:11`); client frames remain in the access layer.
- **User-facing copy:** transport stats names and scheduler diagnostics; no direct UI copy.
- **Lifecycle:** callers construct transports, connect/attach peers, and call `tick()`/`flush()`; `TickScheduler::run()`/`runOnce()` owns phase order and stop (`src/runtime/include/theseed/runtime/TickScheduler.h:77`).

### 7.4 LAY-004: Entity & Space Runtime

- **Implementation:** `Entity.cpp`, `PropertyBlock.cpp`, `PropertyReplication.cpp`, `AOI.cpp`, `Witness.cpp`, `GhostManager.cpp`, `EntityMigration.cpp`, `Space.cpp`, `SpaceRuntime.cpp`, `CellRuntime.cpp`, `LoadProfiler.cpp`, and `BackupTopologyCoordinator.cpp`.
- **Configuration schema:** `SpaceConfig` (`src/runtime/include/theseed/runtime/Space.h:50`), `LoadProfiler::Config` (`src/runtime/include/theseed/runtime/LoadProfiler.h:45`), and `CellRuntime` constructor parameters such as migration TTL (`src/runtime/include/theseed/runtime/CellRuntime.h:30`).
- **Protocol files:** `EntityCall`, `InvocationCodec`, and property-replication encoding provide the runtime-facing method/state payloads.
- **User-facing copy:** AOI/entity events carry semantic event names; this layer has no locale/UI copy.
- **Lifecycle:** CellRuntime attaches/detaches phase-specific tickables; Spaces initialize/shutdown; entities activate/destroy through runtime calls (`src/runtime/include/theseed/runtime/CellRuntime.h:41`).

### 7.5 LAY-005: Application Services

- **Implementation:** `src/core/BaseApp.cpp` and `src/core/CellApp.cpp`, `src/login/LoginApp.cpp`, `src/realm/RealmApp.cpp`, and `src/db/DBApp.cpp`, with shared clients/sessions and protocol modules.
- **Configuration schema:** `BaseApp::Config` (`src/core/include/theseed/core/BaseApp.h:35`), `CellApp::Config` (`src/core/include/theseed/core/CellApp.h:31`), `LoginAppConfig` (`src/login/include/theseed/login/LoginApp.h:36`), `RealmAppConfig` (`src/realm/include/theseed/realm/RealmApp.h:29`), and `DBApp::Config` (`src/db/include/theseed/db/DBApp.h:29`).
- **Protocol files:** `ClientProtocol`/`LoginProtocol` for clients, `DBProtocol` for DB RPC, and machine constants for the control notification leg.
- **User-facing copy:** protocol field/text encoders plus dashboard/ops copy selected by each service; startup banners remain at assembly.
- **Lifecycle:** each service implements `init`/`tick`/`stop`; Base/Cell additionally attach/detach tickables and bind/unbind callbacks (`src/core/BaseApp.cpp:126`, `src/login/LoginApp.cpp:95`, `src/db/DBApp.cpp:197`).

### 7.6 LAY-006: Persistence

- **Implementation:** `FileEntityStore.cpp`, `MySQLConnection.cpp`/`MySQLEntityStore.cpp`, `PostgreSQLConnection.cpp`/`PostgreSQLEntityStore.cpp`, and `RemoteEntityStore.cpp`.
- **Configuration schema:** `DBApp::Config` selects backend and shared DB settings (`src/db/include/theseed/db/DBApp.h:29`); backend-specific forms are `MySQLConnectionConfig`, `PostgreSQLConnectionConfig`, and their store configs.
- **Protocol files:** `DBProtocol` defines the remote persistence RPC; SQL DDL lives in store implementations rather than a schema file.
- **User-facing copy:** DB error strings and DBApp startup banner; no dashboard copy in the store.
- **Lifecycle:** DBApp `init` builds/initializes the selected store and rejects unknown backends, `tick` services RPCs, and `stop` closes the listener/hub (`src/db/DBApp.cpp:123`, `src/db/DBApp.cpp:172`, `src/db/DBApp.cpp:197`).

### 7.7 LAY-007: Observability & Control

- **Implementation:** `src/ops/OpsServer.cpp`/`OpsInspector.cpp`/`DashboardPage.cpp`, `src/control/machine/*`, `src/control/ops/OpsControlCenter.cpp`, and OTLP exporters.
- **Configuration schema:** `OpsServer::Config` (`src/ops/include/theseed/ops/OpsServer.h:26`), `MachineDaemon::Config` and policy structs (`src/control/include/theseed/control/machine/MachineDaemon.h:211`), `OpsControlCenter::Config` (`src/control/include/theseed/control/ops/OpsControlCenter.h:49`), and OTLP exporter configs.
- **Protocol files:** `RuntimeInvocation` reused with machine method constants (`src/control/include/theseed/control/machine/MachineDaemon.h:127`); dashboard markup is embedded by `DashboardPage.cpp`.
- **User-facing copy:** health/inspect JSON, dashboard HTML, machine JSON snapshots, audit fields, and OTLP payload naming.
- **Lifecycle:** OpsServer starts/ticks/stops with its app; MachineDaemon registers on start, reports/ticks, deregisters on graceful stop, and uninstalls exporters (`src/control/machine/MachineDaemon.cpp:348`, `src/control/machine/MachineDaemon.cpp:392`); the center prunes stale nodes (`src/control/ops/OpsControlCenter.cpp:186`).

### 7.8 LAY-008: Tooling & Verification

- **Implementation:** `tools/codegen/main.cpp`/`CSharpEmitter.cpp` and `tools/bots/main.cpp`/`BotsConfig.cpp`/`BotClient.cpp`/`BotManager.cpp`.
- **Configuration schema:** CLI `--defs`, `--output-unity`, and `--namespace` (`tools/codegen/main.cpp:35`); `BotsConfig` defines the key/value bot config contract (`tools/bots/BotsConfig.h:13`).
- **Protocol files:** C# emitter consumes EntityDef; bots reuse Login/Client protocol encoders.
- **User-facing copy:** CLI usage/errors, generated C# header text, and bot stats output.
- **Lifecycle:** short-lived CLIs parse config, load definitions/connect bots, tick to completion/runtime limit, then flush outputs and stop (`tools/codegen/main.cpp:80`, `tools/bots/main.cpp:57`).

### 7.9 Splitting Rationale

1. **Keep assembly disposable.** Main files only parse input and wire dependencies, then call service lifecycle APIs, so process policy can change without changing protocols (`apps/baseapp/main.cpp:117`).
2. **Make transport generic before game semantics.** `RuntimeInvocation` is a component-framed envelope used by both DB and machine traffic, allowing one hub and delivery model (`src/runtime/include/theseed/runtime/RuntimeTransport.h:15`).
3. **Separate phased scheduling from work.** Tick phase names and the scheduler contract are independent of entities; owners register work, preserving deterministic network/timer/entity/sync/flush order (`src/runtime/include/theseed/runtime/TickScheduler.h:16`).
4. **Isolate mutable world state.** Entity/PropertyBlock/AOI/Witness state is distinct from invocation transport and service orchestration, avoiding cross-layer ownership of entities (`src/runtime/include/theseed/runtime/PropertyBlock.h:21`).
5. **Keep process use cases extensible without changing transport.** BaseApp/CellApp/LoginApp/RealmApp/DBApp each compose runtime plus their own protocol/config while assembly remains a consumer (`src/core/include/theseed/core/BaseApp.h:35`).
6. **Store by interface, select backend late.** `IEntityStore` normalizes persistence, while DBApp chooses file/MySQL/PostgreSQL during init and can fall back when a client library is absent (`src/db/DBApp.cpp:123`).
7. **Do not let observation block the tick.** Ops HTTP uses a separate TCP surface and non-blocking tick; metrics render on demand (`src/ops/include/theseed/ops/OpsServer.h:14`).
8. **Keep control authorization explicit and fail closed.** MachineDaemon combines method allowlists with monotonic roles and requires explicit bindings; unbound callers are denied (`src/control/include/theseed/control/machine/AccessControl.h:14`).
9. **Retain telemetry as an opt-in side channel.** OTLP export is disabled by default and installed/uninstalled with daemon lifecycle, so telemetry cannot silently become a startup dependency (`src/control/include/theseed/control/machine/OtlpTraceExporter.h:40`).
10. **Treat definitions and protocols as external tool inputs.** Codegen and bots consume the same XML/client contracts, validating them without linking game service internals (`tools/codegen/main.cpp:68`).
