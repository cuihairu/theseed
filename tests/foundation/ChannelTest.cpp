#include "theseed/foundation/Channel.h"
#include "theseed/foundation/MessageHeader.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <utility>

using theseed::foundation::Bundle;
using theseed::foundation::Channel;
using theseed::foundation::decodeHeader;
using theseed::foundation::IMessageHandler;
using theseed::foundation::MessageDispatcher;
using theseed::foundation::MessageHeader;
using theseed::foundation::MemoryStream;

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

static void testChannelSendDrain() {
    TEST("channel send and drain");

    Channel channel(42);

    Bundle b1;
    b1.beginMessage(1);
    b1.stream().writeInt32(100);
    b1.endMessage();

    channel.send(std::move(b1));

    bool ok = channel.hasPending() && channel.pendingBundleCount() == 1;

    MemoryStream out;
    ok = ok && channel.drain(out);
    ok = ok && !channel.hasPending();
    ok = ok && out.size() > 0;

    if (ok) PASS(); else FAIL("drain failed");
}

static void testChannelMultipleBundles() {
    TEST("channel multiple bundles drain into one stream");

    Channel channel(10);

    for (int i = 0; i < 3; ++i) {
        Bundle b;
        b.beginMessage(static_cast<std::uint16_t>(i + 1));
        b.stream().writeInt32(i * 10);
        b.endMessage();
        channel.send(std::move(b));
    }

    MemoryStream out;
    channel.drain(out);

    out.resetRead();
    for (int i = 0; i < 3; ++i) {
        MessageHeader header;
        bool ok = decodeHeader(header, out);
        if (!ok) { FAIL("decode header " + std::to_string(i)); return; }
        if (header.messageId != static_cast<std::uint16_t>(i + 1)) { FAIL("wrong messageId"); return; }
        auto val = out.readInt32();
        if (val != i * 10) { FAIL("wrong value"); return; }
    }

    PASS();
}

static void testChannelTarget() {
    TEST("channel target component");

    Channel channel(99);
    if (channel.targetComponent() == 99) PASS();
    else FAIL("wrong target");
}

static void testChannelEmptyDrain() {
    TEST("channel empty drain");

    Channel channel(1);
    MemoryStream out;
    bool ok = !channel.drain(out) && out.size() == 0;

    if (ok) PASS(); else FAIL("should be empty");
}

struct TestHandler final : public IMessageHandler {
    std::uint16_t lastMessageId = 0;
    std::int32_t lastValue = 0;
    int callCount = 0;

    bool handleMessage(std::uint16_t messageId, MemoryStream& payload) override {
        lastMessageId = messageId;
        lastValue = payload.readInt32();
        ++callCount;
        return true;
    }
};

static void testDispatcherBasic() {
    TEST("dispatcher register and dispatch");

    MessageDispatcher dispatcher;
    TestHandler handler;

    dispatcher.registerHandler(10, &handler);

    MemoryStream payload;
    payload.writeInt32(42);
    payload.resetRead();

    bool ok = dispatcher.dispatch(10, payload);
    ok = ok && handler.callCount == 1;
    ok = ok && handler.lastMessageId == 10;
    ok = ok && handler.lastValue == 42;

    if (ok) PASS(); else FAIL("dispatch failed");
}

static void testDispatcherNoHandler() {
    TEST("dispatcher no handler returns false");

    MessageDispatcher dispatcher;

    MemoryStream payload;
    bool ok = !dispatcher.dispatch(999, payload);

    if (ok) PASS(); else FAIL("should return false");
}

static void testDispatcherUnregister() {
    TEST("dispatcher unregister");

    MessageDispatcher dispatcher;
    TestHandler handler;

    dispatcher.registerHandler(5, &handler);
    dispatcher.unregisterHandler(5);

    MemoryStream payload;
    bool ok = !dispatcher.dispatch(5, payload);

    if (ok) PASS(); else FAIL("should return false after unregister");
}

static void testDispatcherHandlerCount() {
    TEST("dispatcher handler count");

    MessageDispatcher dispatcher;
    TestHandler h1, h2;

    dispatcher.registerHandler(1, &h1);
    dispatcher.registerHandler(2, &h2);

    bool ok = dispatcher.handlerCount() == 2;

    dispatcher.unregisterHandler(1);
    ok = ok && dispatcher.handlerCount() == 1;

    if (ok) PASS(); else FAIL("count mismatch");
}

static void testChannelAccessors() {
    TEST("channel accessors: nextSequence / overflowPolicy");

    Channel channel(5);
    static_cast<void>(channel.nextSequence());
    static_cast<void>(channel.overflowPolicy());

    PASS();
}

// 背压但策略为默认 BackPressure：不丢弃，队列越过高水位继续增长。
static void testBackPressureDefaultPolicyKeepsBundle() {
    TEST("backpressure with default policy keeps bundles");

    Channel channel(1);
    channel.setWatermark(Channel::Watermark{0, 1});

    Bundle b1;
    b1.beginMessage(1, 0);
    b1.endMessage();
    Bundle b2;
    b2.beginMessage(2, 0);
    b2.endMessage();

    channel.send(std::move(b1));
    channel.send(std::move(b2));  // 已背压 + BackPressure → 不 pop，仅入队

    bool ok = channel.pendingBundleCount() == 2;

    MemoryStream out;
    ok = ok && channel.drain(out);
    ok = ok && out.size() > 0;

    if (ok) PASS(); else FAIL("default policy should not drop, count="
                              + std::to_string(channel.pendingBundleCount()));
}

// 空 bundle（从未 begin）：drain 的 size==0 假臂跳过拷贝。
static void testDrainSkipsEmptyBundle() {
    TEST("drain skips empty bundle stream");

    Channel channel(1);

    Bundle empty;
    channel.send(std::move(empty));  // 无 header 的空流

    Bundle full;
    full.beginMessage(9, 0);
    full.stream().writeInt32(7);
    full.endMessage();
    channel.send(std::move(full));

    MemoryStream out;
    bool ok = channel.drain(out);
    ok = ok && out.size() > 0;

    // 对照组：同一内容但不含空 bundle —— 两者字节数应完全一致。
    Channel reference(1);
    Bundle full2;
    full2.beginMessage(9, 0);
    full2.stream().writeInt32(7);
    full2.endMessage();
    reference.send(std::move(full2));
    MemoryStream refOut;
    static_cast<void>(reference.drain(refOut));
    ok = ok && out.size() == refOut.size();  // 空 bundle 贡献 0 字节

    out.resetRead();
    MessageHeader header;
    ok = ok && decodeHeader(header, out) && header.messageId == 9;

    if (ok) PASS(); else FAIL("empty bundle should contribute no bytes");
}

// messageId 达到 kMaxMessageId 上界的注册/注销/分发全部被拒。
static void testDispatcherBoundaryMessageId() {
    TEST("dispatcher rejects messageId >= 1024");

    MessageDispatcher dispatcher;
    TestHandler handler;

    constexpr std::uint16_t kBound = 1024;
    dispatcher.registerHandler(kBound, &handler);      // 越界注册被忽略
    bool ok = dispatcher.handlerCount() == 0;

    MemoryStream payload;
    ok = ok && !dispatcher.dispatch(kBound, payload);  // 越界分发直接 false

    dispatcher.registerHandler(1, &handler);
    ok = ok && dispatcher.handlerCount() == 1;
    dispatcher.unregisterHandler(kBound);              // 越界注销被忽略，不影响已有
    ok = ok && dispatcher.handlerCount() == 1;
    payload.writeInt32(42);                            // handler 会读一个 int32
    ok = ok && dispatcher.dispatch(1, payload);
    ok = ok && handler.lastValue == 42 && handler.callCount == 1;

    if (ok) PASS(); else FAIL("boundary messageId guards wrong");
}

int main() {
    std::cout << "Channel tests:\n";

    testChannelSendDrain();
    testChannelMultipleBundles();
    testChannelTarget();
    testChannelEmptyDrain();
    testBackPressureDefaultPolicyKeepsBundle();
    testDrainSkipsEmptyBundle();
    testDispatcherBasic();
    testDispatcherNoHandler();
    testDispatcherUnregister();
    testDispatcherBoundaryMessageId();
    testDispatcherHandlerCount();
    testChannelAccessors();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
