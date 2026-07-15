# theseed

以 BigWorld 为源头、KBEngine 为参考实现之一，面向现代 MMO 的分布式游戏服务器引擎。

设计文档见 `docs/design/`（按引擎系统面分 0-foundation ~ 8-reference 八层组织）。实现基线见 `docs/design/0-foundation/01-mvp-architecture-baseline.md`。

## 仓库结构

```
apps/      进程目标：loginapp / realmapp / baseapp / cellapp / dbapp / machine
src/       可复用库（每目录一个静态库）
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
             ops         OpsInspector / OpsServer（只读 inspect + /metrics + /health）
             control     MachineAgent / HostProbe / ProcessSupervisor
             scripting   HotUpdate（受限 L1/L2 热更原型）
tests/     单元/集成测试（按模块组织）
tools/     codegen（C# 客户端代码生成）
res/       实体定义（res/entities/*.xml）
docs/      VitePress 站点 + 完整设计文档
```

## 本地构建

C++23 + CMake ≥ 3.28 + vcpkg manifest mode。

```bash
# 配置（任选一个 preset）
cmake --preset gcc-debug        # 或 clang-debug / mcp msvc-debug(Windows)

# 编译
cmake --build build/gcc-debug

# 测试
ctest --test-dir build/gcc-debug --output-on-failure
```

> MySQL 后端：vcpkg.json 默认声明 `libmysql`。Linux 上 libmysql 构建需要系统前置依赖
> `libtirpc-dev`（`sudo apt-get install -y libtirpc-dev`），且会完整编译 mysql-server 源码
> （首次约 15-30 分钟）。无 libmysql 时自动回退 `FileEntityStore`（条件编译 `THESEED_HAS_MYSQL`）。

## 实现阶段

### Phase A — 现代化运行时最小闭环（已完成）

TickScheduler、Entity/Base-Cell 双体、PropertyBlock、Base↔Cell EntityCall、SingleCell Space、
Witness 同步、FileEntityStore、LoginApp 极简登录。

### Phase B — 补齐在线玩法与基础运维（进行中）

- 十字链表 AOI（`runtime/AOI`）— O(K) 范围查询 ✅
- Controller 系统（MoveToPoint / MoveToEntity）✅
- MySQL 持久化后端（`db/MySQLEntityStore`）— 已实现，待真实环境链接验证
- Redis 会话/限流接入登录链路（`foundation/SessionStore` + `RateLimiter`）✅
- Unity C# 代码生成（`tools/codegen/CSharpEmitter`）✅
- 基础 Metrics / Logger / Tracing ✅
- 只读 Ops inspect（`/metrics` `/health` `/inspect` `/entities`）✅
- Channel / Bundle 状态统计（`TransportStatsCollector`）✅

### Phase C — BigWorld 级系统面原型（已启动，均待端到端集成）

受限脚本热更、迁移路由窗口、BackupTopologyCoordinator、LoadProfiler、基础 Tracing。

## 架构基线要点

- 三个平面分离：Runtime Data Plane / Control Plane / Cross-Realm Async Plane
- 单线程 tick：tick 内只记脏、tick 末统一 flush
- 实体所有权：每个 Entity 只属于一个 tick 线程
- 可靠性语义：Ordered Reliable（EntityCall/控制消息）+ Unordered Lossy（volatile/AOI）
- 存储：标量原生列、VECTOR3 展开、FIXED_DICT/ARRAY 默认 JSON（设计目标）

详见 `docs/design/0-foundation/01-mvp-architecture-baseline.md`。
