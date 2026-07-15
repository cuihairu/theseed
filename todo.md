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

## MySQL 持久化后端（Phase B，已实现，待真实环境验证）

`MySQLEntityStore` 已实现并通过 stub 头文件语法检查（`-fsyntax-only` 全部无 error）。
真实链接验证需要：

- 系统前置依赖：`libtirpc-dev`（提供 `rpc/rpc.h`，libmysql 构建所需）—— `sudo apt-get install -y libtirpc-dev`
- vcpkg 会编译完整 mysql-server 8.0.46 源码（~15-30 分钟）
- 构建成功后 `theseed_db` 启用 `THESEED_HAS_MYSQL`，DBApp 可用 `--backend mysql`
- 无 libmysql 时自动回退 FileEntityStore（条件编译保护）
- 集成测试 `MySqlEntityStoreTest` 需配置 `THESEED_MYSQL_HOST` 等环境变量并连接真实 MySQL 服务

