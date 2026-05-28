#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntityId;
using theseed::runtime::EntitySide;
using theseed::runtime::PropertyDelta;
using theseed::runtime::PropertyId;

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

static std::unique_ptr<EntityDef> makeAvatarDef() {
    auto def = std::make_unique<EntityDef>("Avatar");
    def->addProperty("level", theseed::runtime::PropertyType::Int32);
    def->addProperty("hp", theseed::runtime::PropertyType::Float32);
    def->addProperty("gold", theseed::runtime::PropertyType::Int64);
    return def;
}

// Test: callback fires on local set with correct old/new values
static void testCallbackOnLocalSet() {
    TEST("callback fires on local set with correct old/new values");

    auto def = makeAvatarDef();
    Entity entity(1, EntitySide::Base, *def);

    entity.setProperty<std::int32_t>(0, 5);  // level = 5

    std::int32_t capturedOld = -1;
    std::int32_t capturedNew = -1;
    int callCount = 0;

    entity.onPropertyChanged<std::int32_t>(0,
        [&](Entity& e, std::int32_t oldVal, std::int32_t newVal) {
            capturedOld = oldVal;
            capturedNew = newVal;
            ++callCount;
            static_cast<void>(e);
        });

    entity.setProperty<std::int32_t>(0, 10);  // level = 10

    bool ok = callCount == 1;
    ok = ok && capturedOld == 5;
    ok = ok && capturedNew == 10;
    ok = ok && entity.getProperty<std::int32_t>(0) == 10;

    if (ok) PASS();
    else FAIL("old=" + std::to_string(capturedOld) + " new=" + std::to_string(capturedNew)
              + " count=" + std::to_string(callCount));
}

// Test: callback fires on float property
static void testCallbackOnFloatProperty() {
    TEST("callback fires on float property");

    auto def = makeAvatarDef();
    Entity entity(1, EntitySide::Base, *def);

    entity.setProperty<float>(1, 100.0f);  // hp = 100

    float capturedOld = 0;
    float capturedNew = 0;

    entity.onPropertyChanged<float>(1,
        [&](Entity&, float oldVal, float newVal) {
            capturedOld = oldVal;
            capturedNew = newVal;
        });

    entity.setProperty<float>(1, 75.0f);  // hp = 75

    bool ok = std::abs(capturedOld - 100.0f) < 0.01f;
    ok = ok && std::abs(capturedNew - 75.0f) < 0.01f;

    if (ok) PASS();
    else FAIL("old=" + std::to_string(capturedOld) + " new=" + std::to_string(capturedNew));
}

// Test: no callback when property has no callback registered
static void testNoCallbackWithoutRegistration() {
    TEST("no callback when property has no callback registered");

    auto def = makeAvatarDef();
    Entity entity(1, EntitySide::Base, *def);

    int callCount = 0;
    entity.onPropertyChanged<std::int32_t>(0,
        [&](Entity&, std::int32_t, std::int32_t) { ++callCount; });

    // Set hp (property 1) - no callback registered
    entity.setProperty<float>(1, 50.0f);

    bool ok = callCount == 0;

    // Set level (property 0) - has callback
    entity.setProperty<std::int32_t>(0, 5);
    ok = ok && callCount == 1;

    if (ok) PASS();
    else FAIL("callCount=" + std::to_string(callCount));
}

// Test: callback fires on applyDelta (remote property sync)
static void testCallbackOnApplyDelta() {
    TEST("callback fires on applyDelta (remote sync)");

    auto def = makeAvatarDef();
    Entity entity(1, EntitySide::Base, *def);

    entity.setProperty<std::int32_t>(0, 3);
    entity.setProperty<float>(1, 80.0f);

    std::int32_t capturedOld = 0;
    std::int32_t capturedNew = 0;
    int callCount = 0;

    entity.onPropertyChanged<std::int32_t>(0,
        [&](Entity&, std::int32_t oldVal, std::int32_t newVal) {
            capturedOld = oldVal;
            capturedNew = newVal;
            ++callCount;
        });

    // Simulate remote delta (property sync from cell)
    PropertyDelta delta;
    delta.propertyId = 0;
    delta.value.resize(sizeof(std::int32_t));
    std::int32_t newVal = 15;
    std::memcpy(delta.value.data(), &newVal, sizeof(newVal));

    entity.applyPropertyDelta({&delta, 1}, false);

    bool ok = callCount == 1;
    ok = ok && capturedOld == 3;
    ok = ok && capturedNew == 15;
    ok = ok && entity.getProperty<std::int32_t>(0) == 15;

    if (ok) PASS();
    else FAIL("old=" + std::to_string(capturedOld) + " new=" + std::to_string(capturedNew));
}

// Test: multiple callbacks on different properties
static void testMultiplePropertyCallbacks() {
    TEST("multiple callbacks on different properties");

    auto def = makeAvatarDef();
    Entity entity(1, EntitySide::Base, *def);

    entity.setProperty<std::int32_t>(0, 1);
    entity.setProperty<float>(1, 100.0f);

    int levelCallCount = 0;
    int hpCallCount = 0;
    float capturedHp = 0;

    entity.onPropertyChanged<std::int32_t>(0,
        [&](Entity&, std::int32_t, std::int32_t) { ++levelCallCount; });

    entity.onPropertyChanged<float>(1,
        [&](Entity&, float, float newVal) {
            ++hpCallCount;
            capturedHp = newVal;
        });

    entity.setProperty<std::int32_t>(0, 5);
    entity.setProperty<float>(1, 50.0f);

    bool ok = levelCallCount == 1;
    ok = ok && hpCallCount == 1;
    ok = ok && std::abs(capturedHp - 50.0f) < 0.01f;

    if (ok) PASS();
    else FAIL("level=" + std::to_string(levelCallCount) + " hp=" + std::to_string(hpCallCount));
}

// Test: overwrite existing callback
static void testOverwriteCallback() {
    TEST("overwrite existing callback replaces previous");

    auto def = makeAvatarDef();
    Entity entity(1, EntitySide::Base, *def);

    int firstCount = 0;
    int secondCount = 0;

    entity.onPropertyChanged<std::int32_t>(0,
        [&](Entity&, std::int32_t, std::int32_t) { ++firstCount; });

    entity.setProperty<std::int32_t>(0, 5);
    bool ok = firstCount == 1;

    // Overwrite with new callback
    entity.onPropertyChanged<std::int32_t>(0,
        [&](Entity&, std::int32_t, std::int32_t) { ++secondCount; });

    entity.setProperty<std::int32_t>(0, 10);
    ok = ok && firstCount == 1;  // first callback not called again
    ok = ok && secondCount == 1;

    if (ok) PASS();
    else FAIL("first=" + std::to_string(firstCount) + " second=" + std::to_string(secondCount));
}

// Test: clear callback
static void testClearCallback() {
    TEST("clear callback stops notifications");

    auto def = makeAvatarDef();
    Entity entity(1, EntitySide::Base, *def);

    int callCount = 0;
    entity.onPropertyChanged<std::int32_t>(0,
        [&](Entity&, std::int32_t, std::int32_t) { ++callCount; });

    entity.setProperty<std::int32_t>(0, 5);
    bool ok = callCount == 1;

    entity.setPropertyChangedCallback(0, nullptr);
    entity.setProperty<std::int32_t>(0, 10);
    ok = ok && callCount == 1;  // no additional call

    if (ok) PASS();
    else FAIL("count=" + std::to_string(callCount));
}

// Test: destroy clears all callbacks
static void testDestroyClearsCallbacks() {
    TEST("destroy clears all callbacks");

    auto def = makeAvatarDef();
    Entity entity(1, EntitySide::Base, *def);
    entity.activate();

    int callCount = 0;
    entity.onPropertyChanged<std::int32_t>(0,
        [&](Entity&, std::int32_t, std::int32_t) { ++callCount; });

    entity.setProperty<std::int32_t>(0, 5);
    bool ok = callCount == 1;

    entity.beginDestroy();
    entity.destroy();
    // After destroy, setProperty should still work but callback is gone
    entity.setProperty<std::int32_t>(0, 10);
    ok = ok && callCount == 1;

    if (ok) PASS();
    else FAIL("count=" + std::to_string(callCount));
}

// Test: callback receives entity reference
static void testCallbackReceivesEntityReference() {
    TEST("callback receives correct entity reference");

    auto def = makeAvatarDef();
    Entity entity(42, EntitySide::Base, *def);

    EntityId capturedId = 0;
    entity.onPropertyChanged<std::int32_t>(0,
        [&](Entity& e, std::int32_t, std::int32_t) {
            capturedId = e.id();
        });

    entity.setProperty<std::int32_t>(0, 5);

    bool ok = capturedId == 42;

    if (ok) PASS();
    else FAIL("id=" + std::to_string(capturedId));
}

// Test: applyDelta with multiple properties triggers all relevant callbacks
static void testApplyDeltaMultipleCallbacks() {
    TEST("applyDelta with multiple properties triggers callbacks");

    auto def = makeAvatarDef();
    Entity entity(1, EntitySide::Base, *def);

    entity.setProperty<std::int32_t>(0, 1);
    entity.setProperty<float>(1, 100.0f);

    int levelCalls = 0;
    int hpCalls = 0;

    entity.onPropertyChanged<std::int32_t>(0,
        [&](Entity&, std::int32_t, std::int32_t) { ++levelCalls; });

    entity.onPropertyChanged<float>(1,
        [&](Entity&, float, float) { ++hpCalls; });

    // Apply delta with both properties
    PropertyDelta deltas[2];
    deltas[0].propertyId = 0;
    deltas[0].value.resize(sizeof(std::int32_t));
    std::int32_t newLevel = 10;
    std::memcpy(deltas[0].value.data(), &newLevel, sizeof(newLevel));

    deltas[1].propertyId = 1;
    deltas[1].value.resize(sizeof(float));
    float newHp = 50.0f;
    std::memcpy(deltas[1].value.data(), &newHp, sizeof(newHp));

    entity.applyPropertyDelta(deltas, true);

    bool ok = levelCalls == 1;
    ok = ok && hpCalls == 1;
    ok = ok && entity.getProperty<std::int32_t>(0) == 10;
    ok = ok && std::abs(entity.getProperty<float>(1) - 50.0f) < 0.01f;

    if (ok) PASS();
    else FAIL("level=" + std::to_string(levelCalls) + " hp=" + std::to_string(hpCalls));
}

int main() {
    std::cout << "Property callback tests:\n";

    testCallbackOnLocalSet();
    testCallbackOnFloatProperty();
    testNoCallbackWithoutRegistration();
    testCallbackOnApplyDelta();
    testMultiplePropertyCallbacks();
    testOverwriteCallback();
    testClearCallback();
    testDestroyClearsCallbacks();
    testCallbackReceivesEntityReference();
    testApplyDeltaMultipleCallbacks();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
