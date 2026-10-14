# Repository & Build Layout - monorepo, vcpkg, C++23 baseline

> **English** | [中文](03-repository-and-build-layout.zh.md)

> The goal is not to cram all code into one repository, but to unify shared contracts, dependency baselines, and build entry points.
> This document defines only the repository and build boundaries, not runtime, protocol, or process semantics.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld does it

```
BigWorld's source tree is more like a collection of projects expanded by subsystem:
  - Server, toolchain, and platform adaptation are scattered across different directories
  - Build entry points have long relied on multiple layers of Makefile / CMake / platform scripts
  - Strong system-layer capabilities, but repo-level unified constraints are not modern
```

### How KBEngine does it

```
KBEngine's source organization is more centralized than BigWorld's:
  - server / lib / component boundaries are more intuitive
  - the runtime backbone is easier to locate
  - but third-party dependencies, compilation standards, and module boundaries are still not unified enough
```

### Pros and cons

```
BigWorld's pros:
  - Historically strong ability for subsystems to evolve independently
  - Complete toolchain and system capabilities

BigWorld's cons:
  - Complex build and dependency governance
  - Cross-module contracts drift easily

KBEngine's pros:
  - More intuitive structure
  - Better suited as the starting point for the runtime core

KBEngine's cons:
  - Insufficient repo-level unified constraints
  - Prone to the problem of "the code compiles, but the boundaries are unclear"
```

### theseed's trade-offs

```
theseed adopts monorepo + vcpkg + C++23:
  - One repository unifies shared types, interfaces, and the build baseline
  - One dependency entry point manages third-party library versions
  - One language standard constrains all targets
```

---

## 0. Design Boundary

This document is responsible for:

```
  - Repository root directory layering
  - CMake / vcpkg / compilation standard constraints
  - How shared libraries and executable targets are organized
  - Unified entry points for CI / test / package
```

This document is not responsible for:

```
  - runtime execution semantics
  - protocol compatibility rules
  - fault tolerance / login / ops control plane details
  - world partitioning / AOI / Replication semantics
```

---

## 1. Why Use a monorepo

The purpose of a monorepo is not "a big directory" but to make these things naturally share a single origin:

```
  - Shared header files and interface definitions
  - Versions of runtime libraries and executables
  - Compilation standards for the toolchain and server programs
  - Entry points for testing, code generation, static analysis, and packaging
```

The direct benefits it brings are:

```
1. Avoid cross-repository version drift
2. Avoid maintaining the same protocol separately in multiple repositories
3. Avoid re-wrapping the common toolchain
4. Make it easy to rename and migrate uniformly during refactoring
```

But a monorepo is not without cost:

```
1. A larger build graph
2. Dependency boundaries require more discipline
3. Cross-layer includes for convenience cannot be allowed
4. Explicit target-level isolation is required
```

Conclusion:

```
A monorepo suits theseed,
but only on the premise that "unified directories" must be paired with "unified build and dependency governance".
```

---

## 2. Suggested Root Directory

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

### Directory responsibilities

```
src/
  - Reusable libraries and engine core modules

apps/
  - Executable process targets
  - machine / gateway / login / baseapp / cellapp, etc.

tools/
  - Data ops, diagnostics, packaging, and asset tools

tests/
  - Unit, integration, and compatibility tests

third_party/
  - Only a small amount of supplementary content that cannot be properly expressed through vcpkg
```

---

## 3. Build Baseline

### 3.1 CMake as the unified entry point

It is recommended that the root `CMakeLists.txt` do only three things:

```
  - Define global options and toolchain constraints
  - Aggregate subdirectories by directory
  - Expose unified test / package / install entry points
```

### 3.2 vcpkg as the dependency source

It is recommended to use manifest mode:

```
  - Dependencies are written into vcpkg.json
  - The version baseline is written into vcpkg-configuration.json
  - "What is installed on the system" is not treated as the build truth
```

This avoids:

```
  - Inconsistent dependencies across development machines
  - Divergence between CI and local compilation results
  - Implicit drift of third-party library versions
```

### 3.3 C++23 as the unified language baseline

```
C++23 is not an "optional upgrade item" but the unified language standard of the repository.
```

Suggested constraints:

```
  - All targets uniformly enable cxx_std_23
  - Different dialects between modules are not allowed
  - Use platform specializations only when needed, to avoid language-layer fragmentation
```

---

## 4. vcpkg Strategy

### 4.1 Basic principles

```
1. Dependencies that can be expressed through vcpkg should go through vcpkg first
2. Only when unavoidable should source be added under third_party
3. Custom patches must be traceable
4. Tool dependencies and runtime dependencies are not mixed together
```

### 4.2 Why do it this way

```
The value of vcpkg is not just saving downloads.
More importantly, it turns third-party dependencies from an environment problem into a repository constraint.
```

### 4.3 Risks

```
1. The dependency tree may grow larger
2. Some low-level library versions require extra verification
3. Available packages are not fully consistent across platforms
```

So vcpkg solves only dependency governance and does not replace architectural boundaries.

---

## 5. Recommended Implementation Order

### MVP

```
  - root CMake + vcpkg manifest
  - C++23 baseline
  - apps/machine as the first executable target
  - A minimal skeleton of src/core + src/runtime
```

### Phase 2

```
  - apps/gateway / apps/login
  - tests/unit and tests/integration
  - Basic capabilities of tools/diag
```

### Phase 3

```
  - The complete server-side process family
  - Packaging and release processes
  - A stricter compatibility and performance test matrix
```

---

## 6. In One Sentence

```
theseed's repository layout should first unify "build and dependencies", then unify "modules and processes".
```
