#include "theseed/runtime/IORuntime.h"
#include "theseed/runtime/RuntimeLoop.h"
#include "theseed/runtime/TickScheduler.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
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

struct FailingServiceApp final : IServiceApp {
    bool onStart() override {
        return false;
    }

    void onStop() override {
    }
};

// 首个 tick 就请求停止调度器——让 ServiceApp::run() 的主循环能退出。
struct StopRequestingTickable final : ITickable {
    explicit StopRequestingTickable(TickScheduler& scheduler) : scheduler(scheduler) {}

    void tick(TickContext& context) override {
        static_cast<void>(context);
        scheduler.requestStop();
    }

    TickScheduler& scheduler;
};

// 置位 context.shouldStop——验证 executePhase 的中断分支。
struct ShouldStopTickable final : ITickable {
    void tick(TickContext& context) override {
        context.shouldStop = true;
    }
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

    // maxIoWait getter 返回 setMaxIoWait 设置的值；detach 从调度器摘除
    if (loop.maxIoWait() != std::chrono::milliseconds{1}) {
        return fail("max_io_wait_getter");
    }
    loop.detach(scheduler);

    // start 二次调用走 started_ 早退分支
    {
        auto stub2 = std::make_unique<StubServiceApp>();
        InMemoryIORuntime io2;
        ServiceApp service2(std::move(stub2), std::make_unique<InMemoryIORuntime>(),
                            std::chrono::milliseconds{0});
        if (!service2.start()) {
            return fail("second_start_first");
        }
        if (!service2.start()) {
            return fail("second_start_early_return");
        }
        // 未 start 的 stop() 早退分支
        auto stub3 = std::make_unique<StubServiceApp>();
        ServiceApp service3(std::move(stub3), std::make_unique<InMemoryIORuntime>(),
                            std::chrono::milliseconds{0});
        service3.stop();
    }

    // onStart 失败：start/runOnce/run 都走失败分支
    {
        ServiceApp failing(std::make_unique<FailingServiceApp>(),
                           std::make_unique<InMemoryIORuntime>(),
                           std::chrono::milliseconds{0});
        if (failing.start()) {
            return fail("failing_start_should_fail");
        }
        failing.runOnce();  // start 失败 → 早退
        failing.run();      // start 失败 → 早退
    }

    // run()：主循环跑至 requestStop 后自动 stop
    {
        auto stub4 = std::make_unique<StubServiceApp>();
        auto* stubPtr4 = stub4.get();
        ServiceApp service4(std::move(stub4), std::make_unique<InMemoryIORuntime>(),
                            std::chrono::milliseconds{0});
        StopRequestingTickable stopper(service4.scheduler());
        service4.scheduler().registerTickable(TickPhase::Script, stopper);
        service4.run();
        if (!stubPtr4->stopped) {
            return fail("run_should_stop_app");
        }
    }

    // TickScheduler 边缘分支：unregister 未注册项 / post 空任务 / 只读 getter
    {
        RecordingTickable unregistered("unregistered", events);
        if (scheduler.unregisterTickable(TickPhase::Timer, unregistered)) {
            return fail("unregister_not_registered");
        }

        TickScheduler misc(std::chrono::milliseconds{1});
        misc.post(nullptr);  // 空任务：静默丢弃
        if (misc.tickInterval() != std::chrono::milliseconds{1}) {
            return fail("tick_interval_getter");
        }
        if (misc.running()) {
            return fail("running_before_run");
        }
        misc.runOnce();
        if (misc.lastTickDuration() < Duration::zero()) {
            return fail("last_tick_duration_getter");
        }
    }

    // shouldStop 置位：同 phase 后续 tickable 不再执行，runOnce 结束后请求停止
    {
        TickScheduler stopper_sched(std::chrono::milliseconds{0});
        ShouldStopTickable shouldStop;
        RecordingTickable after("after_should_stop", events);
        stopper_sched.registerTickable(TickPhase::Network, shouldStop);
        stopper_sched.registerTickable(TickPhase::Network, after);

        events.clear();
        const auto ticksBefore = stopper_sched.currentTick();
        stopper_sched.runOnce();
        if (!events.empty()) {
            return fail("should_stop_breaks_phase");
        }
        if (stopper_sched.currentTick() != ticksBefore + 1) {
            return fail("should_stop_tick_advances");
        }

        // 停止请求置位后的 runOnce 早退分支
        stopper_sched.runOnce();
        if (stopper_sched.currentTick() != ticksBefore + 1) {
            return fail("run_once_after_stop_request");
        }
    }

    // 正 tick 间隔的 run()：走 sleep_until 分支后退出
    {
        TickScheduler paced(std::chrono::milliseconds{1});
        StopRequestingTickable pacedStopper(paced);
        paced.registerTickable(TickPhase::Script, pacedStopper);
        paced.run();
        if (paced.running()) {
            return fail("running_after_run");
        }
    }

    // InMemoryIORuntime 边缘分支：wakeup / cancel 未知 token / drain 防御 / 空转等待
    {
        InMemoryIORuntime ioEdge;
        ioEdge.wakeup();  // 置位唤醒标记 → 随后的 runOnce 不阻塞
        ioEdge.runOnce(std::chrono::milliseconds{1});

        if (ioEdge.cancel(theseed::runtime::IoToken{12345})) {
            return fail("cancel_unknown_token");
        }

        std::array<IoCompletion, 2> sink{};
        if (ioEdge.drainCompletions(nullptr, 4) != 0) {
            return fail("drain_null_out");
        }
        if (ioEdge.drainCompletions(sink.data(), 0) != 0) {
            return fail("drain_zero_capacity");
        }

        // 无请求无唤醒：等满 maxWait 后空转返回
        ioEdge.runOnce(std::chrono::milliseconds{5});
    }

    return EXIT_SUCCESS;
}
