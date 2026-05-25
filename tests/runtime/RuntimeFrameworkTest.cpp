#include "theseed/runtime/IORuntime.h"
#include "theseed/runtime/RuntimeLoop.h"
#include "theseed/runtime/TickScheduler.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using theseed::runtime::Duration;
using theseed::runtime::IIORuntime;
using theseed::runtime::InMemoryIORuntime;
using theseed::runtime::IoCompletion;
using theseed::runtime::IoOp;
using theseed::runtime::IoRequest;
using theseed::runtime::IoStatus;
using theseed::runtime::ITickable;
using theseed::runtime::IServiceApp;
using theseed::runtime::RuntimeLoop;
using theseed::runtime::ServiceApp;
using theseed::runtime::TickContext;
using theseed::runtime::TickPhase;
using theseed::runtime::TickScheduler;

namespace {

struct RecordingTickable final : ITickable {
    RecordingTickable(std::string label, std::vector<std::string>& events)
        : label(std::move(label)), events(events) {}

    void tick(TickContext& context) override {
        events.push_back(label + ":" + std::to_string(context.tickIndex));
    }

    std::string label;
    std::vector<std::string>& events;
};

struct PostingTickable final : ITickable {
    PostingTickable(TickScheduler& scheduler, std::vector<std::string>& events)
        : scheduler(scheduler), events(events) {}

    void tick(TickContext& context) override {
        events.push_back("network:" + std::to_string(context.tickIndex));
        if (context.tickIndex == 0) {
            scheduler.post([this] {
                events.push_back("deferred");
            });
        }
    }

    TickScheduler& scheduler;
    std::vector<std::string>& events;
};

int fail(const char* stage) {
    std::cerr << "runtime_test_failed_at=" << stage << '\n';
    return EXIT_FAILURE;
}

struct StubServiceApp final : IServiceApp {
    bool onStart() override {
        started = true;
        return true;
    }

    void onStop() override {
        stopped = true;
    }

    bool started = false;
    bool stopped = false;
};

}  // namespace

int main() {
    std::vector<std::string> events;
    TickScheduler scheduler(std::chrono::milliseconds{0});

    PostingTickable network(scheduler, events);
    RecordingTickable timer("timer", events);
    RecordingTickable entity("entity", events);
    RecordingTickable script("script", events);
    RecordingTickable sync("sync", events);
    RecordingTickable flush("flush", events);

    scheduler.registerTickable(TickPhase::Network, network);
    scheduler.registerTickable(TickPhase::Timer, timer);
    scheduler.registerTickable(TickPhase::Entity, entity);
    scheduler.registerTickable(TickPhase::Script, script);
    scheduler.registerTickable(TickPhase::SyncBuild, sync);
    scheduler.registerTickable(TickPhase::Flush, flush);

    scheduler.runOnce();
    const std::vector<std::string> firstTickExpected{
        "network:0",
        "timer:0",
        "entity:0",
        "script:0",
        "sync:0",
        "flush:0",
    };
    if (events != firstTickExpected) {
        return fail("tick_order");
    }

    if (scheduler.currentTick() != 1) {
        return fail("tick_index");
    }

    events.clear();
    scheduler.runOnce();
    const std::vector<std::string> secondTickExpected{
        "deferred",
        "network:1",
        "timer:1",
        "entity:1",
        "script:1",
        "sync:1",
        "flush:1",
    };
    if (events != secondTickExpected) {
        return fail("deferred_task_order");
    }

    if (scheduler.currentTick() != 2) {
        return fail("tick_advance");
    }

    InMemoryIORuntime ioRuntime;
    const int marker = 42;
    IoRequest request{};
    request.op = IoOp::Read;
    request.userData = const_cast<int*>(&marker);

    const auto token = ioRuntime.submit(request);
    ioRuntime.runOnce(Duration::zero());

    std::array<IoCompletion, 4> completions{};
    const auto completionCount = ioRuntime.drainCompletions(completions.data(), completions.size());
    if (completionCount != 1) {
        return fail("completion_count");
    }

    if (completions[0].token.value != token.value) {
        return fail("completion_token");
    }

    if (completions[0].status != IoStatus::Ok) {
        return fail("completion_status");
    }

    if (completions[0].userData != const_cast<int*>(&marker)) {
        return fail("completion_userdata");
    }

    const int cancelledMarker = 7;
    IoRequest cancelledRequest{};
    cancelledRequest.op = IoOp::Write;
    cancelledRequest.userData = const_cast<int*>(&cancelledMarker);
    const auto cancelledToken = ioRuntime.submit(cancelledRequest);
    if (!ioRuntime.cancel(cancelledToken)) {
        return fail("cancel");
    }

    const auto cancelledCount =
        ioRuntime.drainCompletions(completions.data(), completions.size());
    if (cancelledCount != 1) {
        return fail("cancelled_completion_count");
    }

    if (completions[0].status != IoStatus::Cancelled) {
        return fail("cancelled_status");
    }

    auto stub = std::make_unique<StubServiceApp>();
    auto* stubPtr = stub.get();
    auto ownedRuntime = std::make_unique<InMemoryIORuntime>();
    auto* runtimePtr = ownedRuntime.get();
    ServiceApp serviceApp(
        std::move(stub),
        std::move(ownedRuntime),
        std::chrono::milliseconds{0});

    if (!serviceApp.start()) {
        return fail("service_start");
    }

    if (!stubPtr->started) {
        return fail("service_started_flag");
    }

    IoRequest runtimeRequest{};
    runtimeRequest.op = IoOp::Read;
    runtimeRequest.userData = const_cast<int*>(&marker);
    serviceApp.ioRuntime().submit(runtimeRequest);
    serviceApp.runOnce();

    const auto runtimeCompletionCount =
        serviceApp.runtimeLoop().drain(completions.data(), completions.size());
    if (runtimeCompletionCount != 1) {
        return fail("runtime_loop_completion");
    }

    serviceApp.stop();
    if (!stubPtr->stopped) {
        return fail("service_stopped_flag");
    }

    IoRequest loopRequest{};
    loopRequest.op = IoOp::Read;
    loopRequest.userData = const_cast<int*>(&cancelledMarker);
    serviceApp.ioRuntime().submit(loopRequest);
    if (runtimePtr->pendingRequestCount() != 1) {
        return fail("loop_pending_before");
    }

    RuntimeLoop loop(serviceApp.ioRuntime());
    loop.setMaxIoWait(std::chrono::milliseconds{1});
    TickContext loopContext{};
    loopContext.budget = std::chrono::milliseconds{2};
    loop.tick(loopContext);
    if (runtimePtr->completionCount() != 1) {
        return fail("loop_completion_not_created");
    }
    const auto loopCompletionCount = loop.drain(completions.data(), completions.size());
    if (loopCompletionCount != 1) {
        return fail("runtime_loop_drain");
    }

    return EXIT_SUCCESS;
}
