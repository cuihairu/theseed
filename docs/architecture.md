# theseed Architecture Lineage: From BigWorld and KBEngine to a Modern Implementation

> **English** | [中文](architecture.zh.md)

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
| Missing tests, regression done manually | 115 tests passing green as the commit gate; a 100% red line across the three coverage metrics; a test written but not registered is caught by `-Wunused-function` | `tests/`, CI, `-Werror` |
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
