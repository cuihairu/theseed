#include "theseed/core/TimerWheel.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using theseed::core::TimerHandle;
using theseed::core::TimerWheel;

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

static void testOneShotTimer() {
    TEST("one-shot timer fires");

    TimerWheel wheel(std::chrono::milliseconds{10});

    int fired = 0;
    wheel.addTimer(std::chrono::milliseconds{30}, [&] { ++fired; });

    wheel.advance(std::chrono::milliseconds{10});
    if (fired != 0) { FAIL("fired too early"); return; }

    wheel.advance(std::chrono::milliseconds{20});

    if (fired == 1) PASS();
    else FAIL("expected 1, got " + std::to_string(fired));
}

static void testOneShotTimerExactTick() {
    TEST("one-shot timer fires at exact tick");

    TimerWheel wheel(std::chrono::milliseconds{10});

    int fired = 0;
    wheel.addTimer(std::chrono::milliseconds{10}, [&] { ++fired; });

    wheel.advance(std::chrono::milliseconds{10});

    if (fired == 1) PASS();
    else FAIL("expected 1, got " + std::to_string(fired));
}

static void testPeriodicTimer() {
    TEST("periodic timer fires repeatedly");

    TimerWheel wheel(std::chrono::milliseconds{10});

    int fired = 0;
    wheel.addPeriodic(std::chrono::milliseconds{20}, [&] { ++fired; });

    wheel.advance(std::chrono::milliseconds{20});
    wheel.advance(std::chrono::milliseconds{20});
    wheel.advance(std::chrono::milliseconds{20});

    if (fired == 3) PASS();
    else FAIL("expected 3, got " + std::to_string(fired));
}

static void testCancelTimer() {
    TEST("cancel timer prevents firing");

    TimerWheel wheel(std::chrono::milliseconds{10});

    int fired = 0;
    auto handle = wheel.addTimer(std::chrono::milliseconds{30}, [&] { ++fired; });

    wheel.advance(std::chrono::milliseconds{10});
    wheel.cancel(handle);
    wheel.advance(std::chrono::milliseconds{30});

    if (fired == 0) PASS();
    else FAIL("expected 0, got " + std::to_string(fired));
}

static void testCancelPeriodicTimer() {
    TEST("cancel periodic timer stops future fires");

    TimerWheel wheel(std::chrono::milliseconds{10});

    int fired = 0;
    auto handle = wheel.addPeriodic(std::chrono::milliseconds{20}, [&] { ++fired; });

    wheel.advance(std::chrono::milliseconds{20});
    wheel.cancel(handle);
    wheel.advance(std::chrono::milliseconds{40});

    if (fired == 1) PASS();
    else FAIL("expected 1, got " + std::to_string(fired));
}

static void testMultipleTimers() {
    TEST("multiple timers fire in order");

    TimerWheel wheel(std::chrono::milliseconds{10});

    std::vector<int> order;
    wheel.addTimer(std::chrono::milliseconds{30}, [&] { order.push_back(1); });
    wheel.addTimer(std::chrono::milliseconds{10}, [&] { order.push_back(2); });
    wheel.addTimer(std::chrono::milliseconds{20}, [&] { order.push_back(3); });

    wheel.advance(std::chrono::milliseconds{30});

    bool ok = order.size() == 3 && order[0] == 2 && order[1] == 3 && order[2] == 1;
    if (ok) PASS();
    else FAIL("order wrong");
}

static void testActiveCount() {
    TEST("active count tracks timers");

    TimerWheel wheel(std::chrono::milliseconds{10});

    wheel.addTimer(std::chrono::milliseconds{30}, [] {});
    wheel.addTimer(std::chrono::milliseconds{30}, [] {});

    bool ok = wheel.activeCount() == 2;

    wheel.advance(std::chrono::milliseconds{30});
    ok = ok && wheel.activeCount() == 0;

    if (ok) PASS();
    else FAIL("count mismatch");
}

static void testClear() {
    TEST("clear removes all timers");

    TimerWheel wheel(std::chrono::milliseconds{10});

    int fired = 0;
    wheel.addTimer(std::chrono::milliseconds{30}, [&] { ++fired; });
    wheel.addPeriodic(std::chrono::milliseconds{20}, [&] { ++fired; });

    wheel.clear();

    if (wheel.activeCount() == 0) PASS();
    else FAIL("active count should be 0");
}

static void testTimerHandleValidity() {
    TEST("timer handle validity");

    TimerHandle invalid;
    bool ok = !invalid;

    auto handle = TimerHandle{42, 1};
    ok = ok && static_cast<bool>(handle);
    ok = ok && handle.id == 42 && handle.generation == 1;

    if (ok) PASS();
    else FAIL("handle checks failed");
}

static void testAdvanceLargeStep() {
    TEST("advance with large step fires all due timers");

    TimerWheel wheel(std::chrono::milliseconds{10});

    std::vector<int> order;
    wheel.addTimer(std::chrono::milliseconds{10}, [&] { order.push_back(1); });
    wheel.addTimer(std::chrono::milliseconds{30}, [&] { order.push_back(2); });
    wheel.addTimer(std::chrono::milliseconds{50}, [&] { order.push_back(3); });

    wheel.advance(std::chrono::milliseconds{50});

    bool ok = order.size() == 3 && order[0] == 1 && order[1] == 2 && order[2] == 3;
    if (ok) PASS();
    else FAIL("order wrong");
}

int main() {
    std::cout << "TimerWheel tests:\n";

    testOneShotTimer();
    testOneShotTimerExactTick();
    testPeriodicTimer();
    testCancelTimer();
    testCancelPeriodicTimer();
    testMultipleTimers();
    testActiveCount();
    testClear();
    testTimerHandleValidity();
    testAdvanceLargeStep();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
