#include "theseed/foundation/Bundle.h"
#include "theseed/foundation/MemoryStream.h"
#include "theseed/foundation/MessageHeader.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

using theseed::foundation::Bundle;
using theseed::foundation::decodeHeader;
using theseed::foundation::DeliveryFlag;
using theseed::foundation::MemoryStream;
using theseed::foundation::MessageHeader;

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

static void testSingleMessage() {
    TEST("bundle single message encode/decode");

    Bundle bundle;
    bundle.beginMessage(42, DeliveryFlag::OrderedReliable);
    bundle.stream().writeInt32(100);
    bundle.stream().writeString("hello");
    bundle.endMessage();

    bool ok = bundle.messageCount() == 1;

    auto& stream = bundle.stream();
    stream.resetRead();

    MessageHeader header;
    ok = ok && decodeHeader(header, stream);
    ok = ok && header.messageId == 42;
    ok = ok && header.payloadLength > 0;
    ok = ok && header.sequence == 0;
    ok = ok && header.delivery == DeliveryFlag::OrderedReliable;

    ok = ok && stream.readInt32() == 100;
    ok = ok && stream.readString() == "hello";

    if (ok) PASS(); else FAIL("value mismatch");
}

static void testMultipleMessages() {
    TEST("bundle multiple messages");

    Bundle bundle;

    bundle.beginMessage(1);
    bundle.stream().writeInt32(10);
    bundle.endMessage();

    bundle.beginMessage(2);
    bundle.stream().writeString("world");
    bundle.endMessage();

    bundle.beginMessage(3, DeliveryFlag::UnorderedLossy);
    bundle.stream().writeFloat(3.14f);
    bundle.endMessage();

    bool ok = bundle.messageCount() == 3;

    auto& stream = bundle.stream();
    stream.resetRead();

    // Message 1
    MessageHeader h1;
    ok = ok && decodeHeader(h1, stream);
    ok = ok && h1.messageId == 1 && h1.sequence == 0;
    ok = ok && stream.readInt32() == 10;

    // Message 2
    MessageHeader h2;
    ok = ok && decodeHeader(h2, stream);
    ok = ok && h2.messageId == 2 && h2.sequence == 1;
    ok = ok && stream.readString() == "world";

    // Message 3 (lossy, sequence=0)
    MessageHeader h3;
    ok = ok && decodeHeader(h3, stream);
    ok = ok && h3.messageId == 3 && h3.delivery == DeliveryFlag::UnorderedLossy;
    ok = ok && h3.sequence == 0;

    if (ok) PASS(); else FAIL("value mismatch");
}

static void testPayloadLengthBackfill() {
    TEST("payload length backfill");

    Bundle bundle;
    bundle.beginMessage(99);
    bundle.stream().writeInt32(1);
    bundle.stream().writeInt32(2);
    bundle.stream().writeInt32(3);
    bundle.endMessage();

    auto& stream = bundle.stream();
    stream.resetRead();

    MessageHeader header;
    decodeHeader(header, stream);

    // 3 * sizeof(int32) = 12 bytes
    bool ok = header.payloadLength == 12;
    ok = ok && header.messageId == 99;

    if (ok) PASS(); else FAIL("expected payloadLength=12, got " + std::to_string(header.payloadLength));
}

static void testEmptyPayload() {
    TEST("empty payload message");

    Bundle bundle;
    bundle.beginMessage(7);
    bundle.endMessage();

    auto& stream = bundle.stream();
    stream.resetRead();

    MessageHeader header;
    bool ok = decodeHeader(header, stream);
    ok = ok && header.payloadLength == 0;
    ok = ok && header.messageId == 7;

    if (ok) PASS(); else FAIL("empty payload failed");
}

static void testClear() {
    TEST("bundle clear");

    Bundle bundle;
    bundle.beginMessage(1);
    bundle.stream().writeInt32(42);
    bundle.endMessage();

    bundle.clear();

    bool ok = bundle.messageCount() == 0 && bundle.stream().size() == 0;

    if (ok) PASS(); else FAIL("clear failed");
}

static void testHeaderDecodeInsufficientData() {
    TEST("header decode insufficient data");

    MemoryStream stream;
    stream.writeUint16(1);

    stream.resetRead();
    MessageHeader header;
    bool ok = !decodeHeader(header, stream);

    if (ok) PASS(); else FAIL("should have failed");
}

int main() {
    std::cout << "Bundle tests:\n";

    testSingleMessage();
    testMultipleMessages();
    testPayloadLengthBackfill();
    testEmptyPayload();
    testClear();
    testHeaderDecodeInsufficientData();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
