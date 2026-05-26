#include "theseed/foundation/Channel.h"
#include "theseed/foundation/MessageHeader.h"

#include <cstdint>
#include <iostream>
#include <string>

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

int main() {
    std::cout << "Channel tests:\n";

    testChannelSendDrain();
    testChannelMultipleBundles();
    testChannelTarget();
    testChannelEmptyDrain();
    testDispatcherBasic();
    testDispatcherNoHandler();
    testDispatcherUnregister();
    testDispatcherHandlerCount();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
