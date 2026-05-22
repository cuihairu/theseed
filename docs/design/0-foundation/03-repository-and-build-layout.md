# Repository & Build Layout - monorepo、vcpkg、C++23 基线

> 目标不是把所有代码塞进一个仓库，而是把共享契约、依赖基线和构建入口统一起来。
> 本篇只定义仓库和构建边界，不定义 runtime、协议或进程语义。

---

## 0.5 引擎实现对照与取舍

### BigWorld 是怎么做的

```
BigWorld 的源码树更像按子系统展开的工程集合：
  - 服务端、工具链、平台适配分散在不同目录
  - 构建入口长期依赖多层 Makefile / CMake / 平台脚本
  - 系统层能力强，但仓库级统一约束不算现代
```

### KBEngine 是怎么做的

```
KBEngine 的源码组织比 BigWorld 更集中：
  - server / lib / component 边界更直观
  - 运行时主干更容易定位
  - 但第三方依赖、编译标准和模块边界仍然不够统一
```

### 优缺点

```
BigWorld 的优点：
  - 历史上子系统独立演进能力强
  - 工具链和系统能力完整

BigWorld 的缺点：
  - 构建和依赖治理复杂
  - 跨模块契约容易漂移

KBEngine 的优点：
  - 结构更直观
  - 更适合作为 runtime core 起点

KBEngine 的缺点：
  - 仓库级统一约束不足
  - 容易出现“代码能编过，但边界不清”的问题
```

### theseed 的取舍

```
theseed 采用 monorepo + vcpkg + C++23：
  - 一个仓库统一共享类型、接口和构建基线
  - 一个依赖入口管理第三方库版本
  - 一个语言标准约束所有目标
```

---

## 0. 设计边界

本篇负责：

```
  - 仓库根目录分层
  - CMake / vcpkg / 编译标准约束
  - 共享库与可执行目标的组织方式
  - CI / test / package 的统一入口
```

本篇不负责：

```
  - runtime 运行语义
  - 协议兼容规则
  - 容灾 / 登录 / 运维控制面细节
  - 世界分区 / AOI / Replication 语义
```

---

## 1. 为什么用 monorepo

monorepo 的目的不是“目录大”，而是让这些东西天然同源：

```
  - 共享头文件和接口定义
  - 运行时库与可执行程序的版本
  - 工具链和服务端程序的编译标准
  - 测试、代码生成、静态分析和打包入口
```

它带来的直接收益是：

```
1. 避免跨仓库版本漂移
2. 避免同一协议在多个仓库里各自维护
3. 避免公共工具链被重复封装
4. 方便重构时统一改名和统一迁移
```

但 monorepo 不是没有代价：

```
1. 构建图更大
2. 依赖边界更需要纪律
3. 不能允许为了方便直接跨层 include
4. 必须有明确的 target 级别隔离
```

结论：

```
monorepo 适合 theseed，
但前提是“目录统一”必须配合“构建和依赖治理统一”。
```

---

## 2. 根目录建议

```text
/
  CMakeLists.txt
  CMakePresets.json
  vcpkg.json
  vcpkg-configuration.json
  cmake/
  docs/
  src/
    core/
    runtime/
    transport/
    replication/
    cluster/
    data/
    control/
    world/
    scripting/
  apps/
    machine/
    gateway/
    login/
    baseapp/
    cellapp/
    db/
  tools/
    admin/
    asset/
    data/
    diag/
  tests/
    unit/
    integration/
    compat/
  samples/
  third_party/
```

### 目录职责

```
src/
  - 可复用库和引擎核心模块

apps/
  - 可执行进程目标
  - machine / gateway / login / baseapp / cellapp 等

tools/
  - 数据运维、诊断、打包、资产工具

tests/
  - 单元、集成、兼容测试

third_party/
  - 仅放无法通过 vcpkg 正常表达的少量补充内容
```

---

## 3. 构建基线

### 3.1 CMake 作为统一入口

建议让根 `CMakeLists.txt` 只做三件事：

```
  - 定义全局选项和工具链约束
  - 按目录聚合 subdirectory
  - 暴露统一的测试 / 打包 / 安装入口
```

### 3.2 vcpkg 作为依赖源

建议采用 manifest mode：

```
  - 依赖写入 vcpkg.json
  - 版本基线写入 vcpkg-configuration.json
  - 不把“系统装了什么”当成构建真相
```

这能避免：

```
  - 不同开发机依赖不一致
  - CI 和本地编译结果分裂
  - 第三方库版本被隐式漂移
```

### 3.3 C++23 作为统一语言基线

```
C++23 不是“可选升级项”，而是仓库统一语言标准。
```

建议约束：

```
  - 所有目标统一开启 cxx_std_23
  - 不允许模块之间出现不同 dialect
  - 只在需要时使用平台特化，避免语言层分裂
```

---

## 4. vcpkg 策略

### 4.1 基本原则

```
1. 能用 vcpkg 表达的依赖，优先走 vcpkg
2. 只有不可避免时，才在 third_party 中补充源码
3. 自定义 patch 必须可追溯
4. 不把工具依赖和运行时依赖混在一起
```

### 4.2 为什么这样做

```
vcpkg 的价值不只是省去下载。
它更重要的是把第三方依赖从环境问题变成仓库约束。
```

### 4.3 风险

```
1. 依赖树可能变大
2. 某些底层库版本需要额外验证
3. 不同平台的可用包并不完全一致
```

所以 vcpkg 只解决依赖治理，不替代架构边界。

---

## 5. 推荐落地顺序

### MVP

```
  - root CMake + vcpkg manifest
  - C++23 baseline
  - apps/machine 作为首个可执行目标
  - src/core + src/runtime 的最小骨架
```

### Phase 2

```
  - apps/gateway / apps/login
  - tests/unit 与 tests/integration
  - tools/diag 基础能力
```

### Phase 3

```
  - 完整服务端进程族
  - 打包与发布流程
  - 更严格的兼容和性能测试矩阵
```

---

## 6. 一句话判断

```
theseed 的仓库布局应该先统一“构建和依赖”，再统一“模块和进程”。
```
