# TODO

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
- 后续：PostgreSQL 后端（PostgreSQLEntityStore，实现同一 IEntityStore/IAccountStore 接口）

