# theseed

> **English** | [中文](README.zh.md)

A distributed game server engine for modern MMOs, with BigWorld as its origin and KBEngine as one of its reference implementations.

Design documents live in `docs/design/` (organized into eight layers, 0-foundation ~ 8-reference, by engine system area). The implementation baseline is `docs/design/0-foundation/01-mvp-architecture-baseline.md`.

## Repository Layout

```
apps/      Process targets: loginapp / realmapp / baseapp / cellapp / dbapp / machine
src/       Reusable libraries (one static library per directory)
             foundation  MemoryStream / TimerWheel / ObjectPool / Metrics / Logger / Tracing
                          / RedisProvider / SessionStore / RateLimiter
             runtime     Entity / EntityDef / PropertyBlock / Space / AOI / Witness /
                          Ghost / Controller / TickScheduler / TcpTransport / BackupTopology /
                          LoadProfiler / BehaviorTree / StateMachine
             core        BaseRuntime / BaseApp / CellApp / EntityDefRegistry / EntityDefLoader /
                          FileEntityStore / EntityQuery
             db          DBApp / DBProtocol / RemoteEntityStore / MySQLEntityStore / MySQLConnection
             login       LoginApp / ClientSession / LoginProtocol / SessionToken
             realm       RealmApp
             ops         OpsInspector / OpsServer (read-only inspect + /metrics + /health)
             control     MachineAgent / HostProbe / ProcessSupervisor
             scripting   HotUpdate (restricted L1/L2 hot-update prototype)
tests/     Unit/integration tests (organized by module)
tools/     codegen (C# client code generation)
res/       Entity definitions (res/entities/*.xml)
docs/      VitePress site + full design documents
```

## Local Build

C++23 + CMake ≥ 3.28 + vcpkg manifest mode.

```bash
# Configure (pick one preset)
cmake --preset gcc-debug        # or clang-debug / mcp msvc-debug (Windows)

# Build
cmake --build build/gcc-debug

# Test
ctest --test-dir build/gcc-debug --output-on-failure
```

> MySQL backend: `vcpkg.json` declares `libmysql` by default. On Linux, building libmysql requires the
> system prerequisite `libtirpc-dev` (`sudo apt-get install -y libtirpc-dev`) and compiles the full
> mysql-server source tree (about 15-30 minutes on first build). When libmysql is unavailable, the
> build automatically falls back to `FileEntityStore` (conditional compilation `THESEED_HAS_MYSQL`).

## Implementation Phases

### Phase A — Minimal modern runtime loop (complete)

TickScheduler, Entity/Base-Cell dual bodies, PropertyBlock, Base↔Cell EntityCall, SingleCell Space,
Witness synchronization, FileEntityStore, LoginApp minimal login.

### Phase B — Online gameplay and basic ops (in progress)

- Cross-linked list AOI (`runtime/AOI`) — O(K) range queries ✅
- Controller system (MoveToPoint / MoveToEntity) ✅
- MySQL persistence backend (`db/MySQLEntityStore`) — implemented, pending link verification in a real environment
- Redis session/rate-limiting wired into the login path (`foundation/SessionStore` + `RateLimiter`) ✅
- Unity C# code generation (`tools/codegen/CSharpEmitter`) ✅
- Basic Metrics / Logger / Tracing ✅
- Read-only Ops inspect (`/metrics` `/health` `/inspect` `/entities`) ✅
- Channel / Bundle statistics (`TransportStatsCollector`) ✅

### Phase C — BigWorld-level system-area prototypes (started, all pending end-to-end integration)

Restricted script hot-update, migration routing windows, BackupTopologyCoordinator, LoadProfiler, basic Tracing.

## Architecture Baseline Highlights

- Three-plane separation: Runtime Data Plane / Control Plane / Cross-Realm Async Plane
- Single-threaded tick: mark dirty only inside the tick, flush uniformly at tick end
- Entity ownership: each Entity belongs to exactly one tick thread
- Reliability semantics: Ordered Reliable (EntityCall/control messages) + Unordered Lossy (volatile/AOI)
- Storage: native scalar columns, VECTOR3 expansion, FIXED_DICT/ARRAY as JSON by default (design goal)

See `docs/design/0-foundation/01-mvp-architecture-baseline.md` for details.
