#include "theseed/foundation/ObjectPool.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using theseed::foundation::ObjectPool;
using theseed::foundation::PooledObject;

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

struct SimpleObj {
    int x = 0;
    double y = 0.0;
    std::string name;

    SimpleObj() = default;
    SimpleObj(int x_, double y_, std::string n) : x(x_), y(y_), name(std::move(n)) {}
};

static void testAcquireAndRelease() {
    TEST("acquire and release");

    ObjectPool<SimpleObj> pool(4);
    auto* obj = pool.acquire(42, 3.14, "test");

    bool ok = obj != nullptr && obj->x == 42 && obj->y == 3.14 && obj->name == "test";
    ok = ok && pool.activeCount() == 1;

    pool.release(obj);
    ok = ok && pool.activeCount() == 0;

    if (ok) PASS(); else FAIL("basic acquire/release failed");
}

static void testBlockGrowth() {
    TEST("block growth");

    ObjectPool<int> pool(4);

    std::vector<int*> ptrs;
    for (int i = 0; i < 10; ++i) {
        ptrs.push_back(pool.acquire(i));
    }

    bool ok = pool.activeCount() == 10;
    ok = ok && pool.blockCount() == 3;  // 4 + 4 + 4 = 12 slots

    for (auto* p : ptrs) pool.release(p);
    ok = ok && pool.activeCount() == 0;

    if (ok) PASS(); else FAIL("growth mismatch");
}

static void testPointerStability() {
    TEST("pointer stability after release and reacquire");

    ObjectPool<int> pool(4);

    auto* first = pool.acquire(1);
    pool.release(first);
    auto* second = pool.acquire(2);

    // Should reuse the same slot
    if (first == second && *second == 2) PASS();
    else FAIL("pointer not stable");
}

static void testHighWatermark() {
    TEST("high watermark tracking");

    ObjectPool<int> pool(4);

    std::vector<int*> ptrs;
    for (int i = 0; i < 8; ++i) {
        ptrs.push_back(pool.acquire(i));
    }

    bool ok = pool.highWatermark() == 8;

    for (auto* p : ptrs) pool.release(p);
    ok = ok && pool.highWatermark() == 8;

    if (ok) PASS(); else FAIL("watermark mismatch");
}

static void testResetFunction() {
    TEST("reset function on release");

    int resetCalls = 0;
    ObjectPool<int> pool(4, [&](int& val) {
        ++resetCalls;
        val = 0;
    });

    auto* obj = pool.acquire(42);
    pool.release(obj);

    if (resetCalls == 1) PASS();
    else FAIL("expected 1 reset call, got " + std::to_string(resetCalls));
}

static void testPooledObject() {
    TEST("PooledObject RAII");

    ObjectPool<int> pool(4);

    {
        PooledObject<int> obj(pool, pool.acquire(99));
        bool ok = obj.get() != nullptr && *obj == 99;
        if (!ok) { FAIL("PooledObject get failed"); return; }
    }

    if (pool.activeCount() == 0) PASS();
    else FAIL("expected 0 active after scope exit");
}

static void testPooledObjectMove() {
    TEST("PooledObject move semantics");

    ObjectPool<int> pool(4);

    PooledObject<int> a(pool, pool.acquire(42));
    PooledObject<int> b = std::move(a);

    bool ok = !a && b && *b == 42;

    if (ok) PASS(); else FAIL("move failed");
}

static void testReuseAfterRelease() {
    TEST("object reuse with constructor args");

    ObjectPool<SimpleObj> pool(4);

    auto* a = pool.acquire(1, 2.0, "first");
    pool.release(a);

    auto* b = pool.acquire(3, 4.0, "second");

    bool ok = b->x == 3 && b->y == 4.0 && b->name == "second";
    ok = ok && pool.activeCount() == 1;

    pool.release(b);
    if (ok) PASS(); else FAIL("reuse failed");
}

static void testTotalCount() {
    TEST("total count matches blocks");

    ObjectPool<int> pool(8);

    pool.acquire(1);
    pool.acquire(2);

    bool ok = pool.totalCount() == 8 && pool.activeCount() == 2;

    if (ok) PASS(); else FAIL("total count mismatch");
}

int main() {
    std::cout << "ObjectPool tests:\n";

    testAcquireAndRelease();
    testBlockGrowth();
    testPointerStability();
    testHighWatermark();
    testResetFunction();
    testPooledObject();
    testPooledObjectMove();
    testReuseAfterRelease();
    testTotalCount();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
