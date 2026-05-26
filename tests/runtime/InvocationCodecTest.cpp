#include "theseed/runtime/InvocationCodec.h"
#include "theseed/runtime/PipedTransport.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using theseed::runtime::DeliveryClass;
using theseed::runtime::InvocationCodec;
using theseed::runtime::PipedTransport;
using theseed::runtime::RuntimeInvocation;

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

static void testRoundTripBasic() {
    TEST("encode/decode round trip with all fields");

    RuntimeInvocation original;
    original.entityId = 12345;
    original.targetComponent = 42;
    original.entityType = "Avatar";
    original.method = "onDamage";
    original.deliveryClass = DeliveryClass::ORDERED_RELIABLE;
    original.payload = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};

    auto bytes = InvocationCodec::encode(original);
    auto decoded = InvocationCodec::decode(bytes);

    bool ok = decoded.entityId == original.entityId
           && decoded.targetComponent == original.targetComponent
           && decoded.entityType == original.entityType
           && decoded.method == original.method
           && decoded.deliveryClass == original.deliveryClass
           && decoded.payload.size() == original.payload.size();

    if (ok) PASS();
    else FAIL("field mismatch");
}

static void testRoundTripEmptyPayload() {
    TEST("encode/decode with empty payload and strings");

    RuntimeInvocation original;
    original.entityId = 1;
    original.targetComponent = 2;
    original.entityType = "Item";
    original.method = "tick";
    original.deliveryClass = DeliveryClass::UNORDERED_LOSSY;

    auto bytes = InvocationCodec::encode(original);
    auto decoded = InvocationCodec::decode(bytes);

    bool ok = decoded.entityId == 1
           && decoded.targetComponent == 2
           && decoded.entityType == "Item"
           && decoded.method == "tick"
           && decoded.deliveryClass == DeliveryClass::UNORDERED_LOSSY
           && decoded.payload.empty();

    if (ok) PASS();
    else FAIL("field mismatch");
}

static void testRoundTripLargePayload() {
    TEST("encode/decode with large payload");

    RuntimeInvocation original;
    original.entityId = 999;
    original.targetComponent = 7;
    original.entityType = "World";
    original.method = "sync";
    original.payload.resize(1024);
    for (std::size_t i = 0; i < original.payload.size(); ++i) {
        original.payload[i] = static_cast<std::byte>(i & 0xFF);
    }

    auto bytes = InvocationCodec::encode(original);
    auto decoded = InvocationCodec::decode(bytes);

    bool ok = decoded.entityId == 999
           && decoded.payload.size() == 1024;

    if (ok) {
        bool match = true;
        for (std::size_t i = 0; i < 1024; ++i) {
            if (decoded.payload[i] != static_cast<std::byte>(i & 0xFF)) {
                match = false;
                break;
            }
        }
        if (match) PASS();
        else FAIL("payload content mismatch");
    } else {
        FAIL("header mismatch");
    }
}

static void testEncodedSizeIsDeterministic() {
    TEST("same invocation produces same encoded size");

    RuntimeInvocation inv;
    inv.entityId = 100;
    inv.targetComponent = 5;
    inv.entityType = "NPC";
    inv.method = "patrol";
    inv.deliveryClass = DeliveryClass::ORDERED_RELIABLE;
    inv.payload = {std::byte{0xAA}, std::byte{0xBB}};

    auto bytes1 = InvocationCodec::encode(inv);
    auto bytes2 = InvocationCodec::encode(inv);

    if (bytes1.size() == bytes2.size()) PASS();
    else FAIL("sizes differ");
}

static void testPipedTransportSendReceive() {
    TEST("PipedTransport send/receive via codec");

    PipedTransport transportA(1);
    PipedTransport transportB(2);
    transportA.connect(transportB);

    RuntimeInvocation inv;
    inv.entityId = 500;
    inv.targetComponent = 2;
    inv.entityType = "Player";
    inv.method = "onHeal";
    inv.deliveryClass = DeliveryClass::ORDERED_RELIABLE;
    inv.payload = {std::byte{0x10}, std::byte{0x20}};

    transportA.send(std::move(inv));

    if (transportB.pendingCount() != 1) {
        FAIL("expected 1 pending, got " + std::to_string(transportB.pendingCount()));
        return;
    }

    RuntimeInvocation received;
    auto count = transportB.receive(2, &received, 1);

    bool ok = count == 1
           && received.entityId == 500
           && received.targetComponent == 2
           && received.entityType == "Player"
           && received.method == "onHeal"
           && received.payload.size() == 2
           && received.payload[0] == std::byte{0x10}
           && received.payload[1] == std::byte{0x20};

    if (ok) PASS();
    else FAIL("received data mismatch");
}

static void testPipedTransportFilterByComponent() {
    TEST("PipedTransport filters by target component");

    PipedTransport transportA(1);
    PipedTransport transportB(2);
    transportA.connect(transportB);

    RuntimeInvocation inv1;
    inv1.entityId = 10;
    inv1.targetComponent = 2;
    inv1.entityType = "A";
    inv1.method = "m1";

    RuntimeInvocation inv2;
    inv2.entityId = 11;
    inv2.targetComponent = 99;
    inv2.entityType = "B";
    inv2.method = "m2";

    transportA.send(std::move(inv1));
    transportA.send(std::move(inv2));

    RuntimeInvocation out[4]{};
    auto count = transportB.receive(2, out, 4);

    bool ok = count == 1 && out[0].entityId == 10;
    if (ok) PASS();
    else FAIL("expected 1 message for component 2, got " + std::to_string(count));
}

static void testPipedTransportMultipleMessages() {
    TEST("PipedTransport handles multiple messages in order");

    PipedTransport transportA(1);
    PipedTransport transportB(2);
    transportA.connect(transportB);

    for (std::uint64_t i = 1; i <= 5; ++i) {
        RuntimeInvocation inv;
        inv.entityId = i;
        inv.targetComponent = 2;
        inv.entityType = "E";
        inv.method = "m";
        transportA.send(std::move(inv));
    }

    RuntimeInvocation out[5]{};
    auto count = transportB.receive(2, out, 5);

    bool ok = count == 5;
    for (std::size_t i = 0; i < count && ok; ++i) {
        ok = out[i].entityId == static_cast<std::uint64_t>(i + 1);
    }

    if (ok) PASS();
    else FAIL("order or count mismatch");
}

static void testPipedTransportBidirectional() {
    TEST("PipedTransport bidirectional communication");

    PipedTransport transportA(1);
    PipedTransport transportB(2);
    transportA.connect(transportB);

    RuntimeInvocation toB;
    toB.entityId = 100;
    toB.targetComponent = 2;
    toB.entityType = "X";
    toB.method = "fwd";
    transportA.send(std::move(toB));

    RuntimeInvocation toA;
    toA.entityId = 200;
    toA.targetComponent = 1;
    toA.entityType = "Y";
    toA.method = "rev";
    transportB.send(std::move(toA));

    RuntimeInvocation receivedB;
    transportB.receive(2, &receivedB, 1);
    bool okB = receivedB.entityId == 100;

    RuntimeInvocation receivedA;
    transportA.receive(1, &receivedA, 1);
    bool okA = receivedA.entityId == 200;

    if (okA && okB) PASS();
    else FAIL("bidirectional mismatch");
}

int main() {
    std::cout << "InvocationCodec tests:\n";

    testRoundTripBasic();
    testRoundTripEmptyPayload();
    testRoundTripLargePayload();
    testEncodedSizeIsDeterministic();
    testPipedTransportSendReceive();
    testPipedTransportFilterByComponent();
    testPipedTransportMultipleMessages();
    testPipedTransportBidirectional();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
