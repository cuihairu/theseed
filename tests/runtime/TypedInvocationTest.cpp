#include "theseed/foundation/MemoryStream.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/RuntimeTransport.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <string>

using theseed::foundation::MemoryStream;
using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntityId;
using theseed::runtime::EntitySide;
using theseed::runtime::InMemoryRuntimeTransport;

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

static std::shared_ptr<EntityDef> makeDef() {
    auto def = std::make_shared<EntityDef>("Avatar");
    def->addProperty("hp", theseed::runtime::PropertyType::Int32);
    def->addMethod("onDamage", theseed::runtime::MethodSide::Base);
    def->addMethod("onHeal", theseed::runtime::MethodSide::Cell);
    return def;
}

// Test: EntityCall::callWith serializes multiple typed arguments
static void testCallWithMultipleArgs() {
    TEST("EntityCall::callWith serializes int32 + float + string");

    InMemoryRuntimeTransport transport;
    theseed::runtime::EntityCall call(1, 2, "Avatar");

    auto result = call.callWith(transport, "onDamage",
                                std::int32_t(42), 3.14f, std::string("hello"));

    bool ok = result == theseed::runtime::SendResult::Accepted;

    // Verify received invocation
    theseed::runtime::RuntimeInvocation batch[1];
    auto count = transport.receive(2, batch, 1);
    ok = ok && count == 1;
    ok = ok && batch[0].method == "onDamage";
    ok = ok && batch[0].entityId == 1;
    ok = ok && batch[0].entityType == "Avatar";

    // Deserialize payload via MemoryStream
    MemoryStream ms(batch[0].payload.size());
    ms.writeBytes(batch[0].payload.data(), batch[0].payload.size());
    ms.resetRead();

    auto damage = ms.readInt32();
    auto factor = ms.readFloat();
    auto msg = ms.readString();

    ok = ok && damage == 42;
    ok = ok && factor > 3.13f && factor < 3.15f;
    ok = ok && msg == "hello";

    if (ok) PASS(); else FAIL("args mismatch");
}

// Test: EntityCall::callWith with single argument
static void testCallWithSingleArg() {
    TEST("EntityCall::callWith with single int32");

    InMemoryRuntimeTransport transport;
    theseed::runtime::EntityCall call(5, 3, "Player");

    call.callWith(transport, "levelUp", std::int32_t(100));

    theseed::runtime::RuntimeInvocation batch[1];
    transport.receive(3, batch, 1);

    MemoryStream ms(batch[0].payload.size());
    ms.writeBytes(batch[0].payload.data(), batch[0].payload.size());
    ms.resetRead();

    bool ok = ms.readInt32() == 100;

    if (ok) PASS(); else FAIL("single arg mismatch");
}

// Test: EntityCall::callWith with no arguments
static void testCallWithNoArgs() {
    TEST("EntityCall::callWith with no arguments");

    InMemoryRuntimeTransport transport;
    theseed::runtime::EntityCall call(1, 2, "Avatar");

    call.callWith(transport, "respawn");

    theseed::runtime::RuntimeInvocation batch[1];
    transport.receive(2, batch, 1);

    bool ok = batch[0].payload.empty();

    if (ok) PASS(); else FAIL("expected empty payload, got " + std::to_string(batch[0].payload.size()));
}

// Test: bindStreamMethodHandler receives MemoryStream with typed args
static void testStreamMethodHandler() {
    TEST("bindStreamMethodHandler receives typed arguments");

    auto def = makeDef();
    Entity entity(1, EntitySide::Base, *def);

    std::int32_t receivedDamage = 0;
    float receivedFactor = 0.0f;

    entity.bindStreamMethodHandler("onDamage",
        [&receivedDamage, &receivedFactor](Entity& /*e*/, MemoryStream& ms) {
            receivedDamage = ms.readInt32();
            receivedFactor = ms.readFloat();
        });

    // Build payload with MemoryStream
    MemoryStream ms;
    ms.writeInt32(50);
    ms.writeFloat(2.5f);

    std::vector<std::byte> payload(ms.size());
    std::memcpy(payload.data(), ms.data(), ms.size());

    bool dispatched = entity.dispatchMethod("onDamage", payload);

    bool ok = dispatched;
    ok = ok && receivedDamage == 50;
    ok = ok && receivedFactor > 2.49f && receivedFactor < 2.51f;

    if (ok) PASS(); else FAIL("damage=" + std::to_string(receivedDamage)
                               + " factor=" + std::to_string(receivedFactor));
}

// Test: bindStreamMethodHandler and bindMethodHandler are mutually exclusive
static void testHandlerExclusivity() {
    TEST("stream and raw handlers are mutually exclusive");

    auto def = makeDef();
    Entity entity(1, EntitySide::Base, *def);

    bool rawCalled = false;
    bool streamCalled = false;

    entity.bindMethodHandler("onDamage",
        [&rawCalled](Entity&, std::span<const std::byte>) { rawCalled = true; });

    entity.bindStreamMethodHandler("onDamage",
        [&streamCalled](Entity&, MemoryStream&) { streamCalled = true; });

    std::vector<std::byte> payload;
    entity.dispatchMethod("onDamage", payload);

    bool ok = !rawCalled && streamCalled;

    // Now switch back
    streamCalled = false;
    entity.bindMethodHandler("onDamage",
        [&rawCalled](Entity&, std::span<const std::byte>) { rawCalled = true; });
    entity.dispatchMethod("onDamage", payload);

    ok = ok && rawCalled && !streamCalled;

    if (ok) PASS(); else FAIL("exclusivity violated");
}

// Test: hasMethodHandler detects both types
static void testHasMethodHandler() {
    TEST("hasMethodHandler detects stream and raw handlers");

    auto def = makeDef();
    Entity entity(1, EntitySide::Base, *def);

    bool ok = !entity.hasMethodHandler("onDamage");

    entity.bindStreamMethodHandler("onDamage",
        [](Entity&, MemoryStream&) {});
    ok = ok && entity.hasMethodHandler("onDamage");

    entity.bindMethodHandler("onDamage",
        [](Entity&, std::span<const std::byte>) {});
    ok = ok && entity.hasMethodHandler("onDamage");

    if (ok) PASS(); else FAIL("hasMethodHandler failed");
}

// Test: clearMethodHandlers clears both handler types
static void testClearMethodHandlers() {
    TEST("clearMethodHandlers clears stream and raw handlers");

    auto def = makeDef();
    Entity entity(1, EntitySide::Base, *def);

    entity.bindStreamMethodHandler("onDamage",
        [](Entity&, MemoryStream&) {});
    entity.bindMethodHandler("onDamage",
        [](Entity&, std::span<const std::byte>) {});

    entity.clearMethodHandlers();

    bool ok = !entity.hasMethodHandler("onDamage");

    std::vector<std::byte> payload;
    ok = ok && !entity.dispatchMethod("onDamage", payload);

    if (ok) PASS(); else FAIL("handlers not cleared");
}

// Test: round-trip callWith -> stream handler with multiple types
static void testRoundTripTypedCall() {
    TEST("round-trip: callWith -> stream handler with int32+float+string");

    auto def = makeDef();
    Entity entity(1, EntitySide::Base, *def);

    std::int32_t recvDmg = 0;
    float recvMult = 0.0f;
    std::string recvName;

    entity.bindStreamMethodHandler("onDamage",
        [&recvDmg, &recvMult, &recvName](Entity&, MemoryStream& ms) {
            recvDmg = ms.readInt32();
            recvMult = ms.readFloat();
            recvName = ms.readString();
        });

    InMemoryRuntimeTransport transport;
    theseed::runtime::EntityCall call(1, 1, "Avatar");
    call.callWith(transport, "onDamage", std::int32_t(75), 1.5f, std::string("fireball"));

    theseed::runtime::RuntimeInvocation batch[1];
    transport.receive(1, batch, 1);

    theseed::runtime::RuntimeInvocation inv = batch[0];
    inv.targetComponent = 1;
    entity.dispatchInvocation(inv);

    bool ok = recvDmg == 75;
    ok = ok && recvMult > 1.49f && recvMult < 1.51f;
    ok = ok && recvName == "fireball";

    if (ok) PASS(); else FAIL("dmg=" + std::to_string(recvDmg)
                               + " mult=" + std::to_string(recvMult)
                               + " name=" + recvName);
}

int main() {
    std::cout << "Typed invocation tests:\n";

    testCallWithMultipleArgs();
    testCallWithSingleArg();
    testCallWithNoArgs();
    testStreamMethodHandler();
    testHandlerExclusivity();
    testHasMethodHandler();
    testClearMethodHandlers();
    testRoundTripTypedCall();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
