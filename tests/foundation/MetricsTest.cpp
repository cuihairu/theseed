#include "theseed/foundation/Metrics.h"

#include <atomic>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using theseed::foundation::Counter;
using theseed::foundation::Gauge;
using theseed::foundation::Histogram;
using theseed::foundation::MetricType;
using theseed::foundation::MetricsRegistry;

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

static void testCounterIncrement() {
    TEST("counter increment and value");
    MetricsRegistry::instance().reset();
    auto& c = MetricsRegistry::instance().counter("test_counter", "test counter");
    c.increment();
    c.increment(5);
    bool ok = c.value() == 6;

    auto& same = MetricsRegistry::instance().counter("test_counter");
    ok = ok && (&c == &same);

    if (ok) PASS(); else FAIL("counter arithmetic or identity wrong");
}

static void testCounterMultithread() {
    TEST("counter multithread atomic increments");
    MetricsRegistry::instance().reset();
    auto& c = MetricsRegistry::instance().counter("mt_counter");

    std::vector<std::thread> threads;
    constexpr int kThreads = 4;
    constexpr int kIters = 1000;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&c]() {
            for (int i = 0; i < kIters; ++i) c.increment();
        });
    }
    for (auto& th : threads) th.join();

    if (c.value() == static_cast<std::uint64_t>(kThreads * kIters)) PASS();
    else FAIL("expected " + std::to_string(kThreads * kIters) +
              " got " + std::to_string(c.value()));
}

static void testGaugeSetIncDec() {
    TEST("gauge set/increment/decrement");
    MetricsRegistry::instance().reset();
    auto& g = MetricsRegistry::instance().gauge("test_gauge");

    g.set(100);
    g.increment(50);
    g.decrement(30);
    bool ok = g.value() == 120;

    g.increment(-20);
    ok = ok && g.value() == 100;

    g.set(-5);
    ok = ok && g.value() == -5;

    if (ok) PASS(); else FAIL("gauge arithmetic wrong");
}

static void testHistogramBucket() {
    TEST("histogram bucket assignment");
    MetricsRegistry::instance().reset();
    Histogram::Boundaries bounds{1.0, 5.0, 10.0};
    auto& h = MetricsRegistry::instance().histogram("test_hist", bounds);

    h.observe(0.5);   // <= 1.0
    h.observe(3.0);   // <= 5.0
    h.observe(7.0);   // <= 10.0
    h.observe(15.0);  // > 10.0 -> +Inf bucket

    auto snap = h.snapshot();
    bool ok = snap.count == 4;
    // cumulative: bucketCounts[0] = count(<=1.0) = 1; [1] = 2; [2] = 3; [3] = 4 (+Inf)
    ok = ok && snap.bucketCounts.size() == 4;
    ok = ok && snap.bucketCounts[0] == 1;
    ok = ok && snap.bucketCounts[1] == 2;
    ok = ok && snap.bucketCounts[2] == 3;
    ok = ok && snap.bucketCounts[3] == 4;
    ok = ok && (snap.sum > 25.4 && snap.sum < 25.6);

    if (ok) PASS(); else FAIL("histogram bucket assignment wrong");
}

static void testHistogramOutOfBounds() {
    TEST("histogram out of bounds falls into +Inf");
    MetricsRegistry::instance().reset();
    auto& h = MetricsRegistry::instance().histogram("oob_hist", {1.0, 5.0});

    h.observe(-100.0);
    h.observe(1e9);

    auto snap = h.snapshot();
    bool ok = snap.count == 2;
    ok = ok && snap.bucketCounts[0] == 1;  // <=1.0
    ok = ok && snap.bucketCounts[1] == 1;  // <=5.0
    ok = ok && snap.bucketCounts[2] == 2;  // +Inf holds both

    if (ok) PASS(); else FAIL("out-of-bounds histogram wrong");
}

static void testRegistrySingleton() {
    TEST("registry singleton stability");
    auto& a = MetricsRegistry::instance();
    auto& b = MetricsRegistry::instance();
    if (&a == &b) PASS(); else FAIL("singleton instance differs");
}

static void testRegistryRenderText() {
    TEST("registry renderText format");
    MetricsRegistry::instance().reset();
    auto& c = MetricsRegistry::instance().counter("foo_count", "foo description");
    c.increment(7);
    auto& g = MetricsRegistry::instance().gauge("bar_gauge");
    g.set(42);

    auto text = MetricsRegistry::instance().renderText();
    bool ok = text.find("# HELP foo_count foo description") != std::string::npos;
    ok = ok && text.find("# TYPE foo_count counter") != std::string::npos;
    ok = ok && text.find("foo_count 7") != std::string::npos;
    ok = ok && text.find("# TYPE bar_gauge gauge") != std::string::npos;
    ok = ok && text.find("bar_gauge 42") != std::string::npos;
    ok = ok && text.find("# TYPE script_error_count counter") != std::string::npos;

    if (ok) PASS(); else FAIL("renderText output missing fields");
}

static void testRegistryRenderJson() {
    TEST("registry renderJson format");
    MetricsRegistry::instance().reset();
    auto& c = MetricsRegistry::instance().counter("j_counter", "j");
    c.increment(3);

    auto json = MetricsRegistry::instance().renderJson();
    bool ok = json.find("\"name\":\"j_counter\"") != std::string::npos;
    ok = ok && json.find("\"type\":\"counter\"") != std::string::npos;
    ok = ok && json.find("\"value\":3") != std::string::npos;
    ok = ok && json.find("\"metrics\":[") != std::string::npos;

    if (ok) PASS(); else FAIL("renderJson output missing fields");
}

static void testRegistryReset() {
    TEST("registry reset clears state");
    auto& c = MetricsRegistry::instance().counter("will_reset");
    c.increment(100);

    MetricsRegistry::instance().reset();

    // After reset, the script_error_count placeholder is re-registered.
    auto samples = MetricsRegistry::instance().collect();
    bool ok = samples.size() == 1;
    if (!samples.empty()) {
        ok = ok && samples[0].name == "script_error_count";
    }
    // Re-requesting the counter should be a fresh object with value 0.
    auto& after = MetricsRegistry::instance().counter("will_reset");
    ok = ok && after.value() == 0;

    if (ok) PASS(); else FAIL("reset did not clear state");
}

int main() {
    std::cout << "Metrics tests:\n";

    testCounterIncrement();
    testCounterMultithread();
    testGaugeSetIncDec();
    testHistogramBucket();
    testHistogramOutOfBounds();
    testRegistrySingleton();
    testRegistryRenderText();
    testRegistryRenderJson();
    testRegistryReset();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
