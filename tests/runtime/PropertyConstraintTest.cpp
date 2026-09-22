#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/EntityRef.h"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string>

using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntityId;
using theseed::runtime::EntitySide;
using theseed::runtime::PropertyFlag;
using theseed::runtime::PropertyType;

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

static std::vector<std::byte> makeInt32Value(std::int32_t v) {
    std::vector<std::byte> buf(4);
    std::memcpy(buf.data(), &v, 4);
    return buf;
}

static std::vector<std::byte> makeFloatValue(float v) {
    std::vector<std::byte> buf(4);
    std::memcpy(buf.data(), &v, 4);
    return buf;
}

// Test 1: Int32 min/max constraints - validateProperty
static void testInt32Validate() {
    TEST("Int32: validateProperty with min/max");

    EntityDef def("Avatar");
    def.addProperty("level", PropertyType::Int32, 0, PropertyFlag::None,
                    makeInt32Value(1), makeInt32Value(1), makeInt32Value(100));

    Entity e(1, EntitySide::Base, def);

    bool ok = e.validateProperty<std::int32_t>("level", 1);    // at min
    ok = ok && e.validateProperty<std::int32_t>("level", 50);  // mid range
    ok = ok && e.validateProperty<std::int32_t>("level", 100); // at max
    ok = ok && !e.validateProperty<std::int32_t>("level", 0);  // below min
    ok = ok && !e.validateProperty<std::int32_t>("level", 101); // above max
    ok = ok && !e.validateProperty<std::int32_t>("level", -1); // below min

    if (ok) PASS();
    else FAIL("int32 validation failed");
}

// Test 2: Float min/max constraints
static void testFloatValidate() {
    TEST("Float: validateProperty with min/max");

    EntityDef def("Avatar");
    def.addProperty("hp", PropertyType::Float32, 0, PropertyFlag::None,
                    makeFloatValue(100.0f), makeFloatValue(0.0f), makeFloatValue(999.9f));

    Entity e(1, EntitySide::Cell, def);

    bool ok = e.validateProperty<float>("hp", 0.0f);      // at min
    ok = ok && e.validateProperty<float>("hp", 500.0f);    // mid
    ok = ok && e.validateProperty<float>("hp", 999.9f);    // at max
    ok = ok && !e.validateProperty<float>("hp", -0.1f);    // below min
    ok = ok && !e.validateProperty<float>("hp", 1000.0f);  // above max

    if (ok) PASS();
    else FAIL("float validation failed");
}

// Test 3: trySetProperty accepts valid, rejects invalid
static void testTrySetProperty() {
    TEST("trySetProperty: accepts valid, rejects invalid");

    EntityDef def("Avatar");
    def.addProperty("level", PropertyType::Int32, 0, PropertyFlag::None,
                    makeInt32Value(1), makeInt32Value(1), makeInt32Value(100));

    Entity e(1, EntitySide::Base, def);
    e.activate();

    bool ok = e.trySetProperty<std::int32_t>("level", 50);
    ok = ok && *e.findProperty<std::int32_t>("level") == 50;

    ok = ok && !e.trySetProperty<std::int32_t>("level", 200);
    ok = ok && *e.findProperty<std::int32_t>("level") == 50;  // unchanged

    if (ok) PASS();
    else FAIL("trySet failed");
}

// Test 4: clampProperty clamps to range
static void testClampProperty() {
    TEST("clampProperty: clamps to min/max range");

    EntityDef def("Avatar");
    def.addProperty("level", PropertyType::Int32, 0, PropertyFlag::None,
                    makeInt32Value(1), makeInt32Value(1), makeInt32Value(100));

    Entity e(1, EntitySide::Base, def);

    bool ok = e.clampProperty<std::int32_t>("level", 50) == 50;   // in range
    ok = ok && e.clampProperty<std::int32_t>("level", -10) == 1;  // below min
    ok = ok && e.clampProperty<std::int32_t>("level", 200) == 100; // above max

    if (ok) PASS();
    else FAIL("clamp failed");
}

// Test 5: Unconstrained property always validates
static void testUnconstrainedProperty() {
    TEST("Unconstrained property always validates");

    EntityDef def("Avatar");
    def.addProperty("name", PropertyType::String);
    def.addProperty("count", PropertyType::Int32);

    Entity e(1, EntitySide::Base, def);

    bool ok = e.validateProperty<std::int32_t>("count", -999999);
    ok = ok && e.validateProperty<std::int32_t>("count", 999999);
    ok = ok && e.validateProperty<std::int32_t>("count", 0);

    if (ok) PASS();
    else FAIL("unconstrained should always validate");
}

// Test 6: Only min constraint
static void testOnlyMinConstraint() {
    TEST("Property with only minValue constraint");

    EntityDef def("Avatar");
    def.addProperty("age", PropertyType::Int32, 0, PropertyFlag::None,
                    makeInt32Value(0), makeInt32Value(0), {});  // min=0, no max

    Entity e(1, EntitySide::Base, def);

    bool ok = e.validateProperty<std::int32_t>("age", 0);
    ok = ok && e.validateProperty<std::int32_t>("age", 1000000);
    ok = ok && !e.validateProperty<std::int32_t>("age", -1);

    ok = ok && e.clampProperty<std::int32_t>("age", -50) == 0;
    ok = ok && e.clampProperty<std::int32_t>("age", 50) == 50;

    if (ok) PASS();
    else FAIL("min-only constraint failed");
}

// Test 7: Only max constraint
static void testOnlyMaxConstraint() {
    TEST("Property with only maxValue constraint");

    EntityDef def("Avatar");
    def.addProperty("speed", PropertyType::Float32, 0, PropertyFlag::None,
                    makeFloatValue(1.0f), {}, makeFloatValue(10.0f));  // no min, max=10

    Entity e(1, EntitySide::Base, def);

    bool ok = e.validateProperty<float>("speed", -100.0f);  // no min bound
    ok = ok && e.validateProperty<float>("speed", 10.0f);
    ok = ok && !e.validateProperty<float>("speed", 10.1f);

    ok = ok && std::abs(e.clampProperty<float>("speed", 50.0f) - 10.0f) < 0.01f;
    ok = ok && std::abs(e.clampProperty<float>("speed", -5.0f) - (-5.0f)) < 0.01f;  // not clamped (no min)

    if (ok) PASS();
    else FAIL("max-only constraint failed");
}

// Test 8: Unknown property name
static void testUnknownProperty() {
    TEST("Validation for unknown property returns false");

    EntityDef def("Avatar");
    def.addProperty("level", PropertyType::Int32);

    Entity e(1, EntitySide::Base, def);

    bool ok = !e.validateProperty<std::int32_t>("nonexistent", 5);
    ok = ok && !e.trySetProperty<std::int32_t>("nonexistent", 5);
    ok = ok && e.clampProperty<std::int32_t>("nonexistent", 5) == 5;  // returns input

    if (ok) PASS();
    else FAIL("unknown property handling failed");
}

// Test 9: Validate non-existent property in EntityDef
static void testValidateWithMissingDef() {
    TEST("setProperty bypasses validation (backward compatible)");

    EntityDef def("Avatar");
    def.addProperty("level", PropertyType::Int32, 0, PropertyFlag::None,
                    makeInt32Value(1), makeInt32Value(1), makeInt32Value(100));

    Entity e(1, EntitySide::Base, def);
    e.activate();

    // setProperty bypasses validation — engine-internal sync uses this
    e.setProperty<std::int32_t>("level", 999);
    bool ok = *e.findProperty<std::int32_t>("level") == 999;

    if (ok) PASS();
    else FAIL("setProperty should bypass validation");
}

// Test 10: Float clamp edge cases
static void testFloatClampEdge() {
    TEST("Float: clamp edge cases");

    EntityDef def("Avatar");
    def.addProperty("ratio", PropertyType::Float32, 0, PropertyFlag::None,
                    makeFloatValue(0.5f), makeFloatValue(0.0f), makeFloatValue(1.0f));

    Entity e(1, EntitySide::Base, def);

    bool ok = std::abs(e.clampProperty<float>("ratio", 0.0f) - 0.0f) < 0.001f;
    ok = ok && std::abs(e.clampProperty<float>("ratio", 1.0f) - 1.0f) < 0.001f;
    ok = ok && std::abs(e.clampProperty<float>("ratio", -5.0f) - 0.0f) < 0.001f;
    ok = ok && std::abs(e.clampProperty<float>("ratio", 50.0f) - 1.0f) < 0.001f;

    if (ok) PASS();
    else FAIL("float clamp edge failed");
}

int main() {
    std::cout << "Property constraint tests:\n";

    testInt32Validate();
    testFloatValidate();
    testTrySetProperty();
    testClampProperty();
    testUnconstrainedProperty();
    testOnlyMinConstraint();
    testOnlyMaxConstraint();
    testUnknownProperty();
    testValidateWithMissingDef();
    testFloatClampEdge();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
