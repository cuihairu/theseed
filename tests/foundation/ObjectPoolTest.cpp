#include "theseed/foundation/ObjectPool.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
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

struct alignas(64) OverAlignedObj {
    char payload[64] {};
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

static void testSimpleObjGrowthAndReset() {
    TEST("growth + reset callback on SimpleObj pool");

    // blockSize=1：首次 acquire 时 freeList 为空，走 addBlock 扩容并刷新
    // 高水位；传 resetFn 后 release 走重置回调。字符串取 7 字节字面量，
    // 复用已有 acquire<int, double, const char(&)[7]> 实例。
    ObjectPool<SimpleObj> tiny(1, [](SimpleObj& o) { o.x = 0; });
    auto* p = tiny.acquire(1, 2.0, "second");
    bool ok = p != nullptr && p->name == "second";
    ok = ok && tiny.activeCount() == 1;
    tiny.release(p);
    ok = ok && tiny.activeCount() == 0;

    if (ok) PASS(); else FAIL("growth/reset failed");
}

// 超过 max_align_t 的对齐需求：alignedAlloc 的提升对齐假臂。
static void testOverAlignedType() {
    TEST("over-aligned type gets upgraded alignment");

    int overAlignedResets = 0;
    ObjectPool<OverAlignedObj> pool(2, [&](OverAlignedObj&) { ++overAlignedResets; });
    auto* obj = pool.acquire();
    bool ok = obj != nullptr;
    ok = ok && (reinterpret_cast<std::uintptr_t>(obj) % 64) == 0;  // 64 字节对齐
    pool.release(obj);
    ok = ok && pool.activeCount() == 0 && overAlignedResets == 1;

    if (ok) PASS(); else FAIL("over-aligned allocation misaligned");
}

// release(nullptr) 与默认构造的 PooledObject：守卫假臂均不触碰池。
static void testReleaseGuards() {
    TEST("release(nullptr) and empty PooledObject are no-ops");

    ObjectPool<int> pool(4);
    pool.release(nullptr);  // 空指针早退臂

    {
        PooledObject<int> detached;  // 默认构造：pool_/ptr_ 双空
        static_cast<void>(detached);
    }  // 析构走 if (ptr_ && pool_) 假臂

    bool ok = pool.activeCount() == 0;
    ok = ok && pool.totalCount() == 0;  // 未分配任何块
    if (ok) PASS(); else FAIL("guards should not touch the pool");
}

// lvalue 与 rvalue 混合传参：placement new 转发臂的两种实例。
static void testAcquireForwardsLvalueAndRvalue() {
    TEST("acquire forwards lvalue and rvalue args distinctly");

    ObjectPool<SimpleObj> pool(2);
    std::string lvalueName = "left-value";

    auto* fromLvalue = pool.acquire(1, 1.0, lvalueName);
    bool ok = fromLvalue->name == "left-value";
    pool.release(fromLvalue);

    // rvalue 实例的扩容+水位首发臂：blockSize=1 独立池，首次 rvalue acquire
    // 时 freeList 为空走 addBlock，active 破零走高水位刷新。
    ObjectPool<SimpleObj> rvPool(1);
    auto* firstRvalue = rvPool.acquire(5, 0.5, std::string("rv-growth"));
    ok = ok && firstRvalue->name == "rv-growth";
    ok = ok && rvPool.totalCount() == 1 && rvPool.activeCount() == 1;
    rvPool.release(firstRvalue);
    ok = ok && rvPool.activeCount() == 0;

    auto* fromRvalue = pool.acquire(2, 2.0, std::string("right-value"));
    ok = ok && fromRvalue->name == "right-value";
    pool.release(fromRvalue);

    if (ok) PASS(); else FAIL("forwarding produced wrong values");
}

// release 后再 acquire：active 回到原水位，比较臂走假臂、水位不涨。
static void testHighWatermarkUnchangedAfterRecycle() {
    TEST("recycling does not raise high watermark");

    ObjectPool<int> pool(4);
    std::vector<int*> ptrs;
    for (int i = 0; i < 4; ++i) ptrs.push_back(pool.acquire(i));
    bool ok = pool.highWatermark() == 4;

    for (auto* p : ptrs) pool.release(p);
    for (int i = 0; i < 4; ++i) ptrs[i] = pool.acquire(i);  // active 逐次爬升但不超 4

    ok = ok && pool.highWatermark() == 4;
    for (auto* p : ptrs) pool.release(p);

    if (ok) PASS(); else FAIL("watermark should stay at 4");
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
    testSimpleObjGrowthAndReset();
    testTotalCount();
    testOverAlignedType();
    testReleaseGuards();
    testAcquireForwardsLvalueAndRvalue();
    testHighWatermarkUnchangedAfterRecycle();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
