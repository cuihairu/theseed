# theseed

`theseed` 当前已经从纯文档仓库进入最小实现阶段。

## 当前骨架

- `docs/`: 设计文档与对标分析
- `src/`: 可复用核心库
- `apps/`: 进程目标，当前已包含 `theseed_machine`
- `tests/`: 最小烟雾测试
- `CMakeLists.txt` + `CMakePresets.json`: C++23 + Ninja 构建入口
- `vcpkg.json`: 依赖治理入口，当前保持空依赖基线

## 本地构建

```powershell
cmake --preset clang-debug
cmake --build "D:/workspaces/theseed/build/clang-debug"
ctest --test-dir "D:/workspaces/theseed/build/clang-debug" --output-on-failure
```

## 当前实现边界

- 已完成：monorepo 骨架、`MachineAgent` 核心链路、本机主机摘要、本机进程枚举、受控子进程 `start/stop/restart`、烟雾测试
- 未完成：全局进程管理策略、控制面接入、遥测上报、RPC 接入
