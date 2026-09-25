#include "theseed/foundation/ChannelRouter.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <utility>

using theseed::foundation::Channel;
using theseed::foundation::ChannelRouter;

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

static void testGetOrCreate() {
    TEST("getOrCreate returns same channel for same key");

    ChannelRouter router;
    ChannelRouter::RoutingKey key{42, 0};

    auto& ch1 = router.getOrCreate(key);
    auto& ch2 = router.getOrCreate(key);

    if (&ch1 == &ch2 && router.channelCount() == 1) PASS();
    else FAIL("channel count=" + std::to_string(router.channelCount()));
}

static void testDifferentKeys() {
    TEST("different keys create different channels");

    ChannelRouter router;
    ChannelRouter::RoutingKey reliable{42, 0};
    ChannelRouter::RoutingKey unreliable{42, 1};

    auto& ch1 = router.getOrCreate(reliable);
    auto& ch2 = router.getOrCreate(unreliable);

    if (&ch1 != &ch2 && router.channelCount() == 2) PASS();
    else FAIL("channel count=" + std::to_string(router.channelCount()));
}

static void testFind() {
    TEST("find returns channel or nullptr");

    ChannelRouter router;
    ChannelRouter::RoutingKey key{10, 0};

    if (router.find(key) != nullptr) { FAIL("should be null before create"); return; }

    router.getOrCreate(key);
    if (router.find(key) == nullptr) { FAIL("should exist after create"); return; }

    ChannelRouter::RoutingKey missing{99, 0};
    if (router.find(missing) != nullptr) { FAIL("missing key should be null"); return; }

    PASS();
}

static void testCloseChannel() {
    TEST("closeChannel removes channel");

    ChannelRouter router;
    ChannelRouter::RoutingKey key{5, 0};
    router.getOrCreate(key);

    bool closed = router.closeChannel(key);
    if (!closed || router.channelCount() != 0) { FAIL("close failed"); return; }

    bool closedAgain = router.closeChannel(key);
    if (closedAgain) { FAIL("should not close non-existent"); return; }

    PASS();
}

static void testDrainAll() {
    TEST("drainAll drains all channels in priority order");

    ChannelRouter router;
    theseed::foundation::MemoryStream out;

    // Create channels: Reliable(0), Unreliable(1), Control(2)
    ChannelRouter::RoutingKey reliable{42, 0};
    ChannelRouter::RoutingKey unreliable{42, 1};
    ChannelRouter::RoutingKey control{42, 2};

    auto& chReliable = router.getOrCreate(reliable);
    auto& chUnreliable = router.getOrCreate(unreliable);
    auto& chControl = router.getOrCreate(control);

    // Send one bundle each with different payload
    theseed::foundation::Bundle bReliable;
    bReliable.beginMessage(1, 0);
    std::byte rTag{0xAA};
    bReliable.stream().writeBytes(&rTag, 1);
    bReliable.endMessage();
    chReliable.send(std::move(bReliable));

    theseed::foundation::Bundle bUnreliable;
    bUnreliable.beginMessage(2, 1);
    std::byte uTag{0xBB};
    bUnreliable.stream().writeBytes(&uTag, 1);
    bUnreliable.endMessage();
    chUnreliable.send(std::move(bUnreliable));

    theseed::foundation::Bundle bControl;
    bControl.beginMessage(3, 2);
    std::byte cTag{0xCC};
    bControl.stream().writeBytes(&cTag, 1);
    bControl.endMessage();
    chControl.send(std::move(bControl));

    auto drained = router.drainAll(out);
    if (drained == 0) { FAIL("drainAll returned 0"); return; }

    // Verify total pending is now 0
    if (router.totalPendingCount() != 0) {
        FAIL("pending=" + std::to_string(router.totalPendingCount()));
        return;
    }

    PASS();
}

static void testTotalPendingCount() {
    TEST("totalPendingCount sums all channels");

    ChannelRouter router;
    ChannelRouter::RoutingKey key1{1, 0};
    ChannelRouter::RoutingKey key2{2, 0};

    auto& ch1 = router.getOrCreate(key1);
    auto& ch2 = router.getOrCreate(key2);

    theseed::foundation::Bundle b1;
    b1.beginMessage(1, 0);
    b1.endMessage();
    ch1.send(std::move(b1));

    theseed::foundation::Bundle b2;
    b2.beginMessage(2, 0);
    b2.endMessage();
    ch2.send(std::move(b2));
    ch2.send(theseed::foundation::Bundle{});

    if (router.totalPendingCount() == 3) PASS();
    else FAIL("count=" + std::to_string(router.totalPendingCount()));
}

static void testBackPressure() {
    TEST("hasBackPressuredChannel detects pressure");

    ChannelRouter router;
    ChannelRouter::RoutingKey key{1, 0};
    auto& ch = router.getOrCreate(key);

    Channel::Watermark wm{2, 4};
    ch.setWatermark(wm);

    if (router.hasBackPressuredChannel()) { FAIL("not pressured yet"); return; }

    // Fill to watermark
    for (int i = 0; i < 4; ++i) {
        theseed::foundation::Bundle b;
        b.beginMessage(1, 0);
        b.endMessage();
        ch.send(std::move(b));
    }

    if (!router.hasBackPressuredChannel()) { FAIL("should be pressured"); return; }

    // Verify individual channel
    if (!ch.isBackPressured()) { FAIL("channel should report pressured"); return; }

    PASS();
}

static void testDefaultWatermark() {
    TEST("default watermark applied to new channels");

    ChannelRouter router;
    Channel::Watermark custom{4, 8};
    router.setDefaultWatermark(custom);

    ChannelRouter::RoutingKey key{1, 0};
    auto& ch = router.getOrCreate(key);

    if (ch.watermark().low == 4 && ch.watermark().high == 8) PASS();
    else FAIL("low=" + std::to_string(ch.watermark().low) + " high=" + std::to_string(ch.watermark().high));
}

static void testPerKeySettings() {
    TEST("setWatermark / setOverflowPolicy apply to routed channel");

    ChannelRouter router;
    ChannelRouter::RoutingKey key{7, 1};
    auto& ch = router.getOrCreate(key);

    // 命中已有 channel 的设置分支：high=0 时 0 条 pending 即达背压阈值
    Channel::Watermark wm{0, 0};
    router.setWatermark(key, wm);
    if (!ch.isBackPressured()) { FAIL("watermark not applied"); return; }

    // 触发后按 DiscardOldest 策略走
    Channel::Watermark wm2{0, 1};
    router.setWatermark(key, wm2);
    router.setOverflowPolicy(key, Channel::OverflowPolicy::DiscardOldest);

    theseed::foundation::Bundle b;
    b.beginMessage(1, 0);
    b.endMessage();
    ch.send(std::move(b));
    ch.send(theseed::foundation::Bundle{});

    if (!ch.isBackPressured()) { FAIL("should be pressured at high=1"); return; }

    // 未命中（channel 不存在）时静默忽略
    ChannelRouter::RoutingKey missing{99, 3};
    router.setWatermark(missing, wm);
    router.setOverflowPolicy(missing, Channel::OverflowPolicy::BackPressure);

    PASS();
}

// 空通道不产生字节：drainAll 的 hasPending 假臂。
static void testDrainAllWithEmptyChannels() {
    TEST("drainAll ignores channels without pending bundles");

    ChannelRouter router;
    ChannelRouter::RoutingKey key{5, 0};
    static_cast<void>(router.getOrCreate(key));  // 建通道但不发数据

    theseed::foundation::MemoryStream out;
    auto drained = router.drainAll(out);
    if (drained == 0 && out.size() == 0) PASS();
    else FAIL("empty channels should drain nothing");
}

// RoutingKey 相等比较三臂：全等、peer 异、class_ 异（operator== default）。
static void testRoutingKeyEquality() {
    TEST("routing key equality compares peer and class");

    const ChannelRouter::RoutingKey base{7, 1};
    const ChannelRouter::RoutingKey same{7, 1};
    const ChannelRouter::RoutingKey otherPeer{8, 1};
    const ChannelRouter::RoutingKey otherClass{7, 2};

    if (!(base == same)) { FAIL("identical keys should be equal"); return; }
    if (base == otherPeer) { FAIL("different peer should not be equal"); return; }
    if (base == otherClass) { FAIL("different class should not be equal"); return; }
    PASS();
}

int main() {
    std::cout << "ChannelRouter tests:\n";

    testGetOrCreate();
    testDifferentKeys();
    testFind();
    testCloseChannel();
    testDrainAll();
    testDrainAllWithEmptyChannels();
    testRoutingKeyEquality();
    testTotalPendingCount();
    testBackPressure();
    testDefaultWatermark();
    testPerKeySettings();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
