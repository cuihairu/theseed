#pragma once

#include "theseed/runtime/IORuntime.h"
#include "theseed/runtime/TickScheduler.h"

#include <memory>

namespace theseed::runtime {

class IRuntimePhaseHook {
public:
    virtual ~IRuntimePhaseHook() = default;

    virtual void beforeTick(TickContext& context) = 0;
    virtual void afterTick(const TickContext& context) = 0;
};

class RuntimeLoop final : public ITickable {
public:
    explicit RuntimeLoop(IIORuntime& ioRuntime);

    RuntimeLoop(const RuntimeLoop&) = delete;
    RuntimeLoop& operator=(const RuntimeLoop&) = delete;

    void tick(TickContext& context) override;

    void attach(TickScheduler& scheduler);
    void detach(TickScheduler& scheduler);

    void setMaxIoWait(Duration maxIoWait);
    Duration maxIoWait() const;

    std::size_t drain(IoCompletion* out, std::size_t capacity);

private:
    IIORuntime& ioRuntime_;
    Duration maxIoWait_{};
};

class IServiceApp {
public:
    virtual ~IServiceApp() = default;

    virtual bool onStart() = 0;
    virtual void onStop() = 0;
};

class ServiceApp final {
public:
    ServiceApp(std::unique_ptr<IServiceApp> app,
               std::unique_ptr<IIORuntime> ioRuntime,
               Duration tickInterval = std::chrono::milliseconds{100});

    ServiceApp(const ServiceApp&) = delete;
    ServiceApp& operator=(const ServiceApp&) = delete;

    bool start();
    void stop();
    void runOnce();
    void run();

    TickScheduler& scheduler();
    RuntimeLoop& runtimeLoop();
    IIORuntime& ioRuntime();

private:
    std::unique_ptr<IServiceApp> app_;
    std::unique_ptr<IIORuntime> ioRuntime_;
    TickScheduler scheduler_;
    RuntimeLoop runtimeLoop_;
    bool started_ = false;
};

}  // namespace theseed::runtime
