# Script Debug — Script-Layer Debugging and Execution-State Diagnostics

> English | **中文**

> This document discusses only script runtime debugging; it does not discuss platform telemetry, nor operations control commands.
> Source assessment: the mainline of BigWorld / KBEngine is more about `PyErr_Print`, `traceback`, `watcher`, `reload`.
> theseed separately adds a script execution-state debugging surface on top of that.

---

## 0.5 Engine Implementation Comparison and Trade-offs

### How BigWorld Implements It

```
BigWorld's script debugging leans toward basic capabilities:
  - PyErr_Print
  - traceback
  - watcher-assisted observation
  - reload
```

### How KBEngine Implements It

```
KBEngine likewise mainly revolves around:
  - traceback
  - reload
  - simple observation and logging

It has no independent, complete script execution-state debugging surface.
```

### Pros and Cons

```
Pros of BigWorld / KBEngine:
  - simple
  - less intrusive to the runtime

Shared cons:
  - lack of breakpoints, single-stepping, local variables, and coroutine-state debugging
  - low efficiency in locating complex online script problems
```

### theseed's Trade-offs

```
theseed makes script debugging a separate document,
because it is neither Telemetry
nor the Ops Control Plane,
but a server script execution-state capability.
```

---

## 0. Design Boundary

This document is responsible for:

```
  - breakpoint / step / continue
  - call stack / locals / closure / watch
  - Entity / Space / Realm context
  - coroutine / yield / resume
  - version-consistency validation after hot update
  - debug event auditing and session management
```

This document is not responsible for:

```
  - traces / metrics / logs
  - watcher state-tree queries and online commands
  - hot-update diff analysis and rolling-release orchestration
  - business security validation or client authorization entry points
  - the profiler → load feedback loop
```

Script debugging is a server script execution-state capability,
not OTel, and not the Ops Control Plane.

---

## 1. Why It Is a Separate Document

What BigWorld / KBEngine mainly provide on the script side is:

```
  - exception printing
  - traceback
  - watcher observation
  - reload / reload-like mechanisms
```

These capabilities are enough to locate errors, but they do not amount to a systematic script debugging surface.
What theseed needs to add is:

```
  - breakpoints
  - single-stepping
  - stack and local variables
  - Entity context
  - yield/resume
  - post-hot-update version validation
```

If these are kept mixed into Telemetry, then "seeing what happened" and "controlling how scripts run" get conflated.

---

## 2. Debug Targets

Typical debug targets include:

```
  - ordinary function call frames
  - Entity-bound method frames
  - coroutine suspension points
  - async callback wrapper frames
  - old-version frames after reload
```

Every debug target must carry a minimal context:

```
  - processId
  - entityId
  - entityEpoch
  - scriptVersion
  - scriptPath
  - sourceLine
```

---

## 3. Debug Session Model

```cpp
struct ScriptDebugTarget {
    ProcessId processId;
    EntityId entityId;
    uint64_t entityEpoch;
    uint64_t scriptVersion;
    std::string scriptPath;
};

struct ScriptFrameSnapshot {
    std::string functionName;
    std::string sourcePath;
    uint32_t line = 0;
    std::vector<std::string> locals;
};

class IScriptDebugSession {
public:
    virtual void attach(const ScriptDebugTarget& target) = 0;
    virtual void detach() = 0;
    virtual BreakpointId setBreakpoint(const std::string& path, uint32_t line) = 0;
    virtual void clearBreakpoint(BreakpointId id) = 0;
    virtual void stepInto() = 0;
    virtual void stepOver() = 0;
    virtual void stepOut() = 0;
    virtual void resume() = 0;
    virtual ScriptFrameSnapshot inspectFrame(uint32_t index) = 0;
};
```

The protocol layer may follow a DAP style,
but the semantics must be centered on Entity / Realm / Version,
and one must not directly assume that general-purpose language debugger semantics can be applied losslessly.

---

## 4. Relationship with Hot Update and Async

```
Hot update, migration, and async callbacks all invalidate old frames.
```

Therefore script debugging must explicitly bind:

```
  - entityEpoch
  - scriptVersion
  - suspensionId
```

The rule is simple:

```
As long as the versions are inconsistent, resume must fail or rebuild the context;
it must not silently keep running old frames.
```

The same applies to async callbacks:

```
A late callback may re-enter the stack,
but it must not automatically inherit the old suspended state.
```

---

## 5. Relationship with Lifecycle and Controllers

Script debugging must treat these callbacks as first-class frames:

```
  - Entity lifecycle hooks
  - Controller onStart / onUpdate / onComplete / onCancel
  - timer callbacks
  - async callbacks
```

Suspension points are allowed to fall only on these safe boundaries:

```
  - script function entry
  - hook callback entry
  - controller callback entry
  - timer / future callback entry
```

It is not allowed to insert suspension points into:

```
  - the C++ core synchronous path
  - the middle of a property flush
  - the middle of migration serialization
```

---

## 6. Security and Permissions

Script debugging is allowed only for:

```
  - development state
  - authenticated operations state
  - explicitly authorized local sessions
```

Forbidden:

```
  - entering the debugging surface through a client exposed entry point
  - exposing locals / stack directly to ordinary player requests
  - conflating debug permissions with Telemetry read permissions
```

Debug sessions must be audited:

```
  - attach
  - breakpoint change
  - step
  - resume
  - detach
```

---

## 7. Phased Boundaries

```
MVP:
  - attach / detach
  - traceback / stack dump
  - Entity context inspect
  - coroutine suspension point list

Phase 2:
  - breakpoint / step / continue
  - watch expression
  - conditional breakpoint
  - yield/resume

Phase 3:
  - remote debug adapter
  - multi-process coordinated debugging
  - hot-update-aware session rebinding
```

---

## 8. Comparison with BigWorld / KBEngine / theseed

| Dimension | BigWorld / KBEngine | theseed |
|------|---------------------|---------|
| Script exception location | traceback / watcher / reload assistance | independent Script Debug |
| Breakpoints / single-stepping | no systematic support | yes |
| Entity context | scattered | explicitly modeled |
| yield / resume | basically none | explicitly modeled |
| Hot-update version consistency | none | mandatory validation |
