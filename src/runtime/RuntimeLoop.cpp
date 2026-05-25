#include "theseed/runtime/RuntimeLoop.h"

namespace theseed::runtime {

RuntimeLoop::RuntimeLoop(IIORuntime& ioRuntime) : ioRuntime_(ioRuntime) {}

void RuntimeLoop::tick(TickContext& context) {
    const auto waitTime = maxIoWait_ > Duration::zero() ? maxIoWait_ : context.budget;
    ioRuntime_.runOnce(waitTime);
}

void RuntimeLoop::attach(TickScheduler& scheduler) {
    scheduler.registerTickable(TickPhase::Network, *this);
}

void RuntimeLoop::detach(TickScheduler& scheduler) {
    static_cast<void>(scheduler.unregisterTickable(TickPhase::Network, *this));
}

void RuntimeLoop::setMaxIoWait(Duration maxIoWait) {
    maxIoWait_ = maxIoWait;
}

Duration RuntimeLoop::maxIoWait() const {
    return maxIoWait_;
}

std::size_t RuntimeLoop::drain(IoCompletion* out, std::size_t capacity) {
    return ioRuntime_.drainCompletions(out, capacity);
}

ServiceApp::ServiceApp(std::unique_ptr<IServiceApp> app,
                       std::unique_ptr<IIORuntime> ioRuntime,
                       Duration tickInterval)
    : app_(std::move(app)),
      ioRuntime_(std::move(ioRuntime)),
      scheduler_(tickInterval),
      runtimeLoop_(*ioRuntime_) {
    runtimeLoop_.setMaxIoWait(tickInterval);
    runtimeLoop_.attach(scheduler_);
}

bool ServiceApp::start() {
    if (started_) {
        return true;
    }

    if (!app_ || !app_->onStart()) {
        return false;
    }

    started_ = true;
    return true;
}

void ServiceApp::stop() {
    if (!started_) {
        return;
    }

    scheduler_.requestStop();
    app_->onStop();
    started_ = false;
}

void ServiceApp::runOnce() {
    if (!started_ && !start()) {
        return;
    }

    scheduler_.runOnce();
}

void ServiceApp::run() {
    if (!started_ && !start()) {
        return;
    }

    scheduler_.run();
    stop();
}

TickScheduler& ServiceApp::scheduler() {
    return scheduler_;
}

RuntimeLoop& ServiceApp::runtimeLoop() {
    return runtimeLoop_;
}

IIORuntime& ServiceApp::ioRuntime() {
    return *ioRuntime_;
}

}  // namespace theseed::runtime
