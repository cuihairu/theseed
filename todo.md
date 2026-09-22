# TODO

## 测试覆盖率专项（2026-09-22，行 78.5%→86.9%，gcovr 实测）

基线（gcc-coverage preset + gcovr）：行 78.5%、函数 85.3%、分支 46.0%。
本轮以端到端 TCP 回环测试补齐 0% 文件，过程中发现并修复 5 个真实缺陷：

1. `IRuntimeTransport` 缺 `tick()` 接口——`TransportHub::tick` 只 flush 不 pump，
   TCP 服务端永远读不到入站字节（接口加默认实现，hub tick 调 tick()+flush()）
2. `decodeEntityData` 解码边界异常穿透——畸形网络载荷（截断/垃圾长度）抛
   `runtime_error` 直达服务进程 tick 循环并 terminate（改为边界内返回 false +
   属性数上限 1<<20 防超大分配；DBProtocol 与 FileEntityStore 共用此边界）
3. `OpsServer` 未知路径回 200——监控把打错的路径当成功（改 404，respond 支持
   自定义 statusLine）
4. `DBApp` 用函数级 `static` 分配 peerId——跨实例残留，同进程第二个 DBApp 把
   客户端编成 2 号，回复按 sourceComponent 路由时静默丢弃
5. **DBApp 回复路由与客户端自报身份不一致（协议级）**：服务端按本地分配的
   peerId 注册连接，而客户端回复 target 取请求 sourceComponent——两者只在
   客户端猜中 id=1 时巧合一致，LoginApp(localComponentId=20)→DBApp 的
   生产链路实际全断。修法：`TransportHub::attachServerTransport`，服务端
   连接由首条入站消息的 sourceComponent 自学习注册（`awaitingIdentity_`）

新增测试（全部真 TCP 回环，非 mock）：

- `DBAppE2ETest`（21 项）：file 双库门控场景，覆盖 init/tick/accept/全部
  db.* 分发/账号线性扫描回退/畸形载荷错误分支/OpsServer 四端点/端口冲突；
  mysql、postgresql 后端经环境变量门控在真库上验证
- `RealmAppE2ETest`（7 项）：QueryRealms 往返/未知消息存活/ops 端点/断开清理
- `LoginAppE2ETest`（16 项）：null+限流+SessionStore、password 回退、db 鉴权
  三条路径（auto-register/正确/错误密码，后台线程驱动 DBApp）
- `MachineSnapshotCodecTest`：text/JSON 格式化、全部转义分支、空进程列表
- `BackupTopologyCoordinatorTest` 增补 processes() 排序与 reset()
- `PostgreSQLEntityStoreTest` 补自隔离清理（表名含大写需引号），消除对
  新鲜库的隐式依赖

当前覆盖（gcovr，含双库门控测试）：行 86.9%、函数 90.9%、分支 52.2%；
≥25 行源文件全部 ≥75%。下一批缺口：`ProcessSupervisor`（fork 真进程的
supervise 重启路径）、`EntityQuery`、`CellRuntime`/`BaseRuntime` 分支覆盖。

## 遗留事项

当前只优先实现 `MachineAgent -> HostProbe -> ProcessSupervisor -> snapshot` 核心链路。
以下事项暂不进入当前实现：

- `LocalHostProbe` 的高精度 CPU 采样稳定化，当前 CLI 两次短窗口采样仍可能得到 `0.00`
- 网络流量统计与多网卡聚合
- 非受控全局进程的策略化管理与权限边界
- 端口占用扫描与二进制版本探测
- Linux / macOS 的等价主机探针完整实现与验证
- `MachineAgent` 的 RPC 输出与控制面注册
- 与 `Ops Control Plane` 的注册、上报和审计对接
- 与 `Telemetry` 的指标、日志、trace 联动
- 更完整的单元测试与跨平台 CI 构建矩阵

## MySQL 持久化后端（Phase B，已在真实环境验证通过 2026-09-22）

`MySQLEntityStore` 已通过真实 MySQL 8.0.46 实例的完整验证：
`MySqlEntityStoreTest` 22/22 通过，`theseed_dbapp --backend mysql` 启动冒烟通过。
真实编译暴露并修复了三个客户端层 bug（prepared 查询悬垂指针、my_bool 移除、
整数参数按 BLOB 绑定被严格模式拒绝），见提交 a27ef6b。

真实构建前置依赖（vcpkg 编译 mysql-server 8.0.46 源码，~15-30 分钟）：

- `libtirpc-dev`：提供 `rpc/rpc.h`（glibc 2.28+ 已移除 sunrpc 头）
- `libncurses-dev`：提供系统 `term.h`。MySQL 的 CMake 在配置期检测
  `CHECK_INCLUDE_FILES(term.h)` 时不会带 vcpkg 的 include 路径，缺失时
  内嵌 libedit 编译失败（GCC 15 默认 C23 把隐式声明当硬错误）
- 配置命令：`cmake --preset gcc-debug -D CMAKE_TOOLCHAIN_FILE=~/vcpkg/scripts/buildsystems/vcpkg.cmake -D VCPKG_MANIFEST_INSTALL=ON`
  （`CMAKE_TOOLCHAIN_FILE` 只在全新缓存的首配生效）

本地真实验证环境（podman，rootless）：

```
podman run -d --name theseed-mysql -e MYSQL_ROOT_PASSWORD=theseed_test_pw \
  -e MYSQL_DATABASE=theseed_test -p 127.0.0.1:13306:3306 docker.io/library/mysql:8.0

THESEED_MYSQL_HOST=127.0.0.1 THESEED_MYSQL_PORT=13306 THESEED_MYSQL_USER=root \
THESEED_MYSQL_PASSWORD=theseed_test_pw THESEED_MYSQL_DATABASE=theseed_test \
./build/gcc-debug/tests/db/theseed_mysql_store_test
```

- 无 libmysql 时自动回退 FileEntityStore（条件编译保护），集成测试自动跳过

## PostgreSQL 持久化后端（Phase B，已在真实环境验证通过 2026-09-22）

`PostgreSQLEntityStore` 已通过真实 PostgreSQL 16 实例的完整验证：
`PostgreSQLEntityStoreTest` 24/24 通过，全量 ctest 96/96，
`theseed_dbapp --backend postgresql` 启动冒烟通过（`pg_stat_activity` 确认真实 libpq 连接），
`--backend mysql` 回归冒烟通过（两个后端共用 `db*` 连接配置）。

设计要点：

- 与 `MySQLEntityStore` 实现同一 `IEntityStore`/`IAccountStore` 接口，存储模型
  （每类型一张 `tbl_<Type>` 表 + BYTEA 序列化 blob）与 MySQL 版完全一致
- `allocId` 用 PG 方言单语句原子完成：
  `INSERT ... ON CONFLICT ... DO UPDATE ... RETURNING next_id`（无需 MySQL 的
  LAST_INSERT_ID(expr) 两步），首 id 为 1
- `DBApp` 经 `--backend file|mysql|postgresql` 选择后端；CLI 同时提供 `--mysql-*`
  与 `--pg-*` 参数别名（写同一组 `db*` 配置字段，`--backend postgresql` 未指定
  端口时默认 5432）；构建期缺 libpq 时回退 FileEntityStore
- 共享参数模型 `SqlParam`（`u64()` 整数 / `str()` 文本 / 字节串 / `null()`）：
  PG text 协议下字节串按 `\x...` 十六进制 + `::bytea` cast，文本参数必须原样
  传（按 bytea 编码绑进 VARCHAR 列会存成 `"\\x..."` 字面文本）
- PG 18 客户端头不再导出 `BYTEAOID`（移入 server 头 `catalog/pg_type_d.h`），
  代码本地定义 `kByteaOid = 17`（目录表冻结常量）

真实构建前置依赖（vcpkg 编译 PostgreSQL 18.4 客户端源码，meson + ninja）：

- `bison` + `flex`：PG 源码需要生成 parser/lexer。无 sudo 时可用
  `apt-get download bison flex && dpkg -x <deb> ~/.local/tools/` 解包，
  配 `PATH` 与 `BISON_PKGDATADIR=~/.local/tools/usr/share/bison`
  （bison 的数据文件编译期固定在 /usr/share/bison，缺失时报
  `m4sugar.m4: cannot open`）
- 配置命令同 MySQL 一节

本地真实验证环境（podman，rootless）：

```
podman run -d --name theseed-postgres -e POSTGRES_PASSWORD=theseed_test_pw \
  -e POSTGRES_DB=theseed_test -p 127.0.0.1:13307:5432 docker.io/library/postgres:16

THESEED_PG_HOST=127.0.0.1 THESEED_PG_PORT=13307 THESEED_PG_USER=postgres \
THESEED_PG_PASSWORD=theseed_test_pw THESEED_PG_DATABASE=theseed_test \
./build/gcc-debug/tests/db/theseed_pg_store_test
```

- 无 libpq 时自动回退 FileEntityStore（条件编译保护），集成测试自动跳过

