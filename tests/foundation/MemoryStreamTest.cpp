#include "theseed/foundation/MemoryStream.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

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

static void testWriteReadIntegers() {
    TEST("write/read integers");

    MemoryStream stream;
    stream.writeInt8(-42);
    stream.writeUint8(200);
    stream.writeInt16(-12345);
    stream.writeUint16(65000);
    stream.writeInt32(-100000);
    stream.writeUint32(3000000000U);
    stream.writeInt64(-99999999999LL);
    stream.writeUint64(18000000000000000000ULL);

    stream.resetRead();

    bool ok = true;
    ok = ok && stream.readInt8() == -42;
    ok = ok && stream.readUint8() == 200;
    ok = ok && stream.readInt16() == -12345;
    ok = ok && stream.readUint16() == 65000;
    ok = ok && stream.readInt32() == -100000;
    ok = ok && stream.readUint32() == 3000000000U;
    ok = ok && stream.readInt64() == -99999999999LL;
    ok = ok && stream.readUint64() == 18000000000000000000ULL;

    if (ok) PASS(); else FAIL("value mismatch");
}

static void testWriteReadFloats() {
    TEST("write/read floats");

    MemoryStream stream;
    stream.writeFloat(3.14f);
    stream.writeDouble(2.718281828);

    stream.resetRead();

    bool ok = true;
    ok = ok && std::abs(stream.readFloat() - 3.14f) < 0.001f;
    ok = ok && std::abs(stream.readDouble() - 2.718281828) < 0.000001;

    if (ok) PASS(); else FAIL("value mismatch");
}

static void testWriteReadBool() {
    TEST("write/read bool");

    MemoryStream stream;
    stream.writeBool(true);
    stream.writeBool(false);

    stream.resetRead();

    bool ok = true;
    ok = ok && stream.readBool() == true;
    ok = ok && stream.readBool() == false;

    if (ok) PASS(); else FAIL("value mismatch");
}

static void testWriteReadString() {
    TEST("write/read string");

    MemoryStream stream;
    stream.writeString("hello world");
    stream.writeString("");
    stream.writeString("unicode: \xe4\xbd\xa0\xe5\xa5\xbd");

    stream.resetRead();

    bool ok = true;
    ok = ok && stream.readString() == "hello world";
    ok = ok && stream.readString() == "";
    ok = ok && stream.readString() == "unicode: \xe4\xbd\xa0\xe5\xa5\xbd";

    if (ok) PASS(); else FAIL("value mismatch");
}

static void testWriteReadBytes() {
    TEST("write/read raw bytes");

    MemoryStream stream;
    const char data[] = "raw bytes test";
    stream.writeBytes(data, sizeof(data));

    stream.resetRead();

    char out[sizeof(data)] = {};
    stream.readBytes(out, sizeof(data));

    if (std::memcmp(data, out, sizeof(data)) == 0) PASS();
    else FAIL("byte mismatch");
}

static void testReadOverflow() {
    TEST("read overflow throws");

    MemoryStream stream;
    stream.writeInt32(42);

    stream.resetRead();
    stream.readInt32();

    bool threw = false;
    try {
        stream.readInt8();
    } catch (const std::runtime_error&) {
        threw = true;
    }

    if (threw) PASS(); else FAIL("expected exception");
}

static void testReadSkip() {
    TEST("read skip");

    MemoryStream stream;
    stream.writeInt32(1);
    stream.writeInt32(2);
    stream.writeInt32(3);

    stream.resetRead();
    stream.readSkip(4);

    if (stream.readInt32() == 2) PASS();
    else FAIL("value mismatch after skip");
}

static void testWriteSkip() {
    TEST("write skip");

    MemoryStream stream;
    stream.writeInt32(42);
    stream.writeSkip(4);
    stream.writeInt32(99);

    stream.resetRead();
    auto first = stream.readInt32();
    stream.readSkip(4);
    auto second = stream.readInt32();

    bool ok = first == 42 && second == 99;
    if (ok) PASS(); else FAIL("value mismatch");
}

static void testClear() {
    TEST("clear");

    MemoryStream stream;
    stream.writeInt32(42);
    stream.clear();

    bool ok = stream.size() == 0 && stream.readPos() == 0 && stream.writePos() == 0;

    if (ok) PASS(); else FAIL("clear failed");
}

static void testMoveSemantics() {
    TEST("move semantics");

    MemoryStream source;
    source.writeInt32(123);
    source.resetRead();

    MemoryStream dest(std::move(source));

    bool ok = dest.readInt32() == 123;
    ok = ok && source.size() == 0;

    if (ok) PASS(); else FAIL("move failed");
}

static void testAutoGrow() {
    TEST("auto grow capacity");

    MemoryStream stream(16);
    auto initialCap = stream.capacity();

    for (int i = 0; i < 100; ++i) {
        stream.writeInt32(i);
    }

    stream.resetRead();
    bool ok = true;
    for (int i = 0; i < 100; ++i) {
        ok = ok && stream.readInt32() == i;
    }

    ok = ok && stream.capacity() > initialCap;

    if (ok) PASS(); else FAIL("growth or value mismatch");
}

static void testReadRemaining() {
    TEST("read remaining");

    MemoryStream stream;
    stream.writeInt32(1);
    stream.writeInt32(2);

    stream.resetRead();
    stream.readInt32();

    auto remaining = stream.readRemaining();
    if (remaining == 4) PASS();
    else FAIL("expected 4 remaining, got " + std::to_string(remaining));
}

int main() {
    std::cout << "MemoryStream tests:\n";

    testWriteReadIntegers();
    testWriteReadFloats();
    testWriteReadBool();
    testWriteReadString();
    testWriteReadBytes();
    testReadOverflow();
    testReadSkip();
    testWriteSkip();
    testClear();
    testMoveSemantics();
    testAutoGrow();
    testReadRemaining();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
