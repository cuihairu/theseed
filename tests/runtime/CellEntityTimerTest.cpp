#include "theseed/runtime/CellRuntime.h"
#include "theseed/foundation/TimerWheel.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/RuntimeTransport.h"
#include "theseed/runtime/SpaceRuntime.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

using theseed::foundation::TimerHandle;
using theseed::runtime::CellRuntime;
using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntityId;
using theseed::runtime::EntitySide;
using theseed::runtime::InMemoryRuntimeTransport;
using theseed::runtime::SingleCellTopology;
using theseed::runtime::Space;
using theseed::runtime::SpaceRuntime;
using theseed::runtime::Vector3;

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name)                                              \
    do {                                                        \
        std::cout << "  " << (name) << "... " << std::flush;    \
    } while (0)

#define PASS()                                                  \
    do {                                                        \
        std::cout << "OK\n";                                    \
        ++testsPassed;                                          \
    } while (0)

#define FAIL(msg)                                               \
    do {                                                        \
        std::cout << "FAILED: " << (msg) << "\n";               \
        ++testsFailed;                                          \
    } while (0)

static std::unique_ptr<CellRuntime> makeCellRuntime() {
    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto space = std::make_unique<Space>(1, "default", std::make_unique<SingleCellTopology>(1));
    space->initialize();
    auto spaceRuntime = std::make_unique<SpaceRuntime>(std::move(space));
    return std::make_unique<CellRuntime>(std::move(spaceRuntime), transport, 2);
}

static void testAddTimer() {
    TEST("add timer fires after delay");

    auto rt = makeCellRuntime();
    bool fired = false;
    rt->addTimer(std::chrono::milliseconds{100}, [&fired]() { fired = true; });

    theseed::runtime::TickContext ctx;
    ctx.deltaTime = std::chrono::milliseconds{150};
    rt->tick(ctx);

    if (fired) PASS(); else FAIL("timer did not fire");
}

static void testCancelTimer() {
    TEST("cancel timer prevents callback");

    auto rt = makeCellRuntime();
    bool fired = false;
    auto handle = rt->addTimer(std::chrono::milliseconds{100}, [&fired]() { fired = true; });

    rt->cancelTimer(handle);

    theseed::runtime::TickContext ctx;
    ctx.deltaTime = std::chrono::milliseconds{200};
    rt->tick(ctx);

    if (!fired) PASS(); else FAIL("cancelled timer fired");
}

static void testTimerInTick() {
    TEST("timer fires during tick progression");

    auto rt = makeCellRuntime();
    int count = 0;
    rt->addTimer(std::chrono::milliseconds{30}, [&count]() { ++count; });

    theseed::runtime::TickContext ctx;
    ctx.deltaTime = std::chrono::milliseconds{20};

    rt->tick(ctx);
    bool ok = count == 0;

    ctx.deltaTime = std::chrono::milliseconds{20};
    rt->tick(ctx);
    ok = ok && count == 1;

    ctx.deltaTime = std::chrono::milliseconds{20};
    rt->tick(ctx);
    ok = ok && count == 1;

    if (ok) PASS(); else FAIL("tick progression failed, count=" + std::to_string(count));
}

int main() {
    std::cout << "CellEntityTimer tests:\n";

    testAddTimer();
    testCancelTimer();
    testTimerInTick();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
