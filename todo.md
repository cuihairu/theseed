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
