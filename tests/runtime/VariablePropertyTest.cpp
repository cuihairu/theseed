#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"

#include <cstring>
#include <iostream>
#include <memory>
#include <string>

using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntitySide;
using theseed::runtime::PropertyDelta;
using theseed::runtime::PropertyId;
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

static std::unique_ptr<EntityDef> makeMixedDef() {
    auto def = std::make_unique<EntityDef>("Avatar");
    def->addProperty("level", PropertyType::Int32);
    def->addProperty("name", PropertyType::String);
    def->addProperty("hp", PropertyType::Float32);
    def->addProperty("inventory", PropertyType::Blob);
    return def;
}

// Test: string property get/set via PropertyBlock
static void testStringGetSet() {
    TEST("string property get/set");

    auto def = makeMixedDef();
    Entity entity(1, EntitySide::Base, *def);

    entity.setString(1, "Alice");
    auto val = entity.getString(1);

    bool ok = val == "Alice";

    if (ok) PASS();
    else FAIL("expected 'Alice', got '" + std::string(val) + "'");
}

// Test: string property get/set by name
static void testStringGetSetByName() {
    TEST("string property get/set by name");

    auto def = makeMixedDef();
    Entity entity(1, EntitySide::Base, *def);

    bool setOk = entity.setString("name", "Bob");
    auto val = entity.findString("name");

    bool ok = setOk && val == "Bob";

    if (ok) PASS();
    else FAIL("setOk=" + std::to_string(setOk) + " val='" + std::string(val) + "'");
}

// Test: string property overwrites previous value
static void testStringOverwrite() {
    TEST("string property overwrites previous value");

    auto def = makeMixedDef();
    Entity entity(1, EntitySide::Base, *def);

    entity.setString(1, "first");
    entity.setString(1, "second");
    auto val = entity.getString(1);

    bool ok = val == "second";

    if (ok) PASS();
    else FAIL("expected 'second', got '" + std::string(val) + "'");
}

// Test: string property dirty tracking
static void testStringDirtyTracking() {
    TEST("string property dirty tracking");

    auto def = makeMixedDef();
    Entity entity(1, EntitySide::Base, *def);

    bool dirtyBeforeSet = entity.isPropertyDirty(1);
    entity.setString(1, "test");
    bool dirtyAfterSet = entity.isPropertyDirty(1);
    entity.clearDirtyFlags();
    bool dirtyAfterClear = entity.isPropertyDirty(1);

    bool ok = !dirtyBeforeSet && dirtyAfterSet && !dirtyAfterClear;

    if (ok) PASS();
    else FAIL("before=" + std::to_string(dirtyBeforeSet)
              + " after=" + std::to_string(dirtyAfterSet)
              + " cleared=" + std::to_string(dirtyAfterClear));
}

// Test: string property change callback
static void testStringChangeCallback() {
    TEST("string property change callback");

    auto def = makeMixedDef();
    Entity entity(1, EntitySide::Base, *def);

    entity.setString(1, "old");

    std::string capturedOld;
    std::string capturedNew;
    int callCount = 0;

    entity.setPropertyChangedCallback(1,
        [&](Entity&, PropertyId, const std::byte* oldVal, const std::byte* newVal, std::size_t size) {
            capturedOld = std::string(reinterpret_cast<const char*>(oldVal), size);
            capturedNew = std::string(reinterpret_cast<const char*>(newVal), size);
            ++callCount;
        });

    entity.setString(1, "new");

    bool ok = callCount == 1;
    ok = ok && entity.getString(1) == "new";

    if (ok) PASS();
    else FAIL("count=" + std::to_string(callCount));
}

// Test: blob property get/set
static void testBlobGetSet() {
    TEST("blob property get/set");

    auto def = makeMixedDef();
    Entity entity(1, EntitySide::Base, *def);

    std::vector<std::byte> data = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
    entity.setBlob(3, data);
    auto val = entity.getBlob(3);

    bool ok = val.size() == 3;
    ok = ok && val[0] == std::byte{0x01};
    ok = ok && val[1] == std::byte{0x02};
    ok = ok && val[2] == std::byte{0x03};

    if (ok) PASS();
    else FAIL("size=" + std::to_string(val.size()));
}

// Test: blob property dirty tracking
static void testBlobDirtyTracking() {
    TEST("blob property dirty tracking");

    auto def = makeMixedDef();
    Entity entity(1, EntitySide::Base, *def);

    std::vector<std::byte> data = {std::byte{0xAA}};
    entity.setBlob(3, data);

    bool ok = entity.isPropertyDirty(3);

    if (ok) PASS();
    else FAIL("expected dirty");
}

// Test: buildFullSnapshot includes variable-sized properties
static void testFullSnapshotIncludesStrings() {
    TEST("buildFullSnapshot includes string and blob");

    auto def = makeMixedDef();
    Entity entity(1, EntitySide::Base, *def);

    entity.setProperty<std::int32_t>(0, 42);
    entity.setString(1, "hero");
    entity.setProperty<float>(2, 100.0f);
    std::vector<std::byte> blobData = {std::byte{0xFF}, std::byte{0x00}};
    entity.setBlob(3, blobData);

    auto snapshot = entity.buildFullPropertySnapshot();

    // Should have 4 properties
    bool ok = snapshot.size() == 4;
    if (!ok) {
        FAIL("expected 4 deltas, got " + std::to_string(snapshot.size()));
        return;
    }

    // Verify fixed property
    std::int32_t level = 0;
    std::memcpy(&level, snapshot[0].value.data(), sizeof(level));
    ok = ok && level == 42;

    // Verify string
    std::string name(reinterpret_cast<const char*>(snapshot[1].value.data()),
                     snapshot[1].value.size());
    ok = ok && name == "hero";

    // Verify blob
    ok = ok && snapshot[3].value.size() == 2;
    ok = ok && snapshot[3].value[0] == std::byte{0xFF};

    if (ok) PASS();
    else FAIL("snapshot content mismatch");
}

// Test: buildDirtyDelta includes only dirty variable properties
static void testDirtyDeltaIncludesDirtyStrings() {
    TEST("buildDirtyDelta includes only dirty string properties");

    auto def = makeMixedDef();
    Entity entity(1, EntitySide::Base, *def);

    entity.setProperty<std::int32_t>(0, 10);
    entity.setString(1, "dirty_name");
    // hp and inventory not set

    auto deltas = entity.buildDirtyPropertyDelta();

    bool hasLevel = false;
    bool hasName = false;
    bool hasHp = false;
    bool hasInventory = false;

    for (const auto& d : deltas) {
        if (d.propertyId == 0) hasLevel = true;
        if (d.propertyId == 1) hasName = true;
        if (d.propertyId == 2) hasHp = true;
        if (d.propertyId == 3) hasInventory = true;
    }

    bool ok = hasLevel && hasName && !hasHp && !hasInventory;

    if (ok) PASS();
    else FAIL("level=" + std::to_string(hasLevel) + " name=" + std::to_string(hasName)
              + " hp=" + std::to_string(hasHp) + " inv=" + std::to_string(hasInventory));
}

// Test: applyDelta with string data
static void testApplyDeltaString() {
    TEST("applyDelta with string data");

    auto def = makeMixedDef();
    Entity entity(1, EntitySide::Base, *def);

    std::string original = "original";
    PropertyDelta delta;
    delta.propertyId = 1;
    const auto* origBegin = reinterpret_cast<const std::byte*>(original.data());
    delta.value.assign(origBegin, origBegin + original.size());

    entity.applyPropertyDelta({&delta, 1}, false);

    auto val = entity.getString(1);
    bool ok = val == "original";

    if (ok) PASS();
    else FAIL("expected 'original', got '" + std::string(val) + "'");
}

// Test: applyDelta with blob data
static void testApplyDeltaBlob() {
    TEST("applyDelta with blob data");

    auto def = makeMixedDef();
    Entity entity(1, EntitySide::Base, *def);

    PropertyDelta delta;
    delta.propertyId = 3;
    delta.value = {std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};

    entity.applyPropertyDelta({&delta, 1}, true);

    auto val = entity.getBlob(3);
    bool ok = val.size() == 4
           && val[0] == std::byte{0xDE}
           && val[1] == std::byte{0xAD}
           && val[2] == std::byte{0xBE}
           && val[3] == std::byte{0xEF};

    if (ok) PASS();
    else FAIL("size=" + std::to_string(val.size()));
}

// Test: applyDelta with markDirty sets dirty flag on string
static void testApplyDeltaMarksDirty() {
    TEST("applyDelta marks string dirty when markDirty=true");

    auto def = makeMixedDef();
    Entity entity(1, EntitySide::Base, *def);

    PropertyDelta delta;
    delta.propertyId = 1;
    const std::byte dataBytes[] = {std::byte{'d'}, std::byte{'a'}, std::byte{'t'}, std::byte{'a'}};
    delta.value.assign(dataBytes, dataBytes + 4);

    entity.applyPropertyDelta({&delta, 1}, true);
    bool ok = entity.isPropertyDirty(1);

    if (ok) PASS();
    else FAIL("expected dirty");
}

// Test: empty string returns empty string_view
static void testEmptyString() {
    TEST("empty string returns empty string_view");

    auto def = makeMixedDef();
    Entity entity(1, EntitySide::Base, *def);

    auto val = entity.getString(1);
    bool ok = val.empty();

    if (ok) PASS();
    else FAIL("expected empty, got '" + std::string(val) + "'");
}

// Test: empty blob returns empty span
static void testEmptyBlob() {
    TEST("empty blob returns empty span");

    auto def = makeMixedDef();
    Entity entity(1, EntitySide::Base, *def);

    auto val = entity.getBlob(3);
    bool ok = val.empty();

    if (ok) PASS();
    else FAIL("expected empty, got size=" + std::to_string(val.size()));
}

// Test: findString returns empty for non-existent property
static void testFindStringNonExistent() {
    TEST("findString returns empty for non-existent property");

    auto def = makeMixedDef();
    Entity entity(1, EntitySide::Base, *def);

    auto val = entity.findString("nonexistent");
    bool ok = val.empty();

    if (ok) PASS();
    else FAIL("expected empty");
}

// Test: setString by name returns false for non-existent property
static void testSetStringNonExistent() {
    TEST("setString by name returns false for non-existent");

    auto def = makeMixedDef();
    Entity entity(1, EntitySide::Base, *def);

    bool ok = !entity.setString("nonexistent", "value");

    if (ok) PASS();
    else FAIL("expected false");
}

// Test: mixed fixed + variable snapshot round-trip
static void testMixedSnapshotRoundTrip() {
    TEST("mixed fixed+variable snapshot round-trip");

    auto def = makeMixedDef();
    Entity src(1, EntitySide::Base, *def);
    Entity dst(2, EntitySide::Base, *def);

    src.setProperty<std::int32_t>(0, 99);
    src.setString(1, "roundtrip_name");
    src.setProperty<float>(2, 42.5f);
    std::vector<std::byte> blob = {std::byte{0xCA}, std::byte{0xFE}};
    src.setBlob(3, blob);

    auto snapshot = src.buildFullPropertySnapshot();
    dst.applyPropertyDelta(snapshot);

    bool ok = dst.getProperty<std::int32_t>(0) == 99;
    ok = ok && dst.getString(1) == "roundtrip_name";

    float hp = dst.getProperty<float>(2);
    ok = ok && hp > 42.4f && hp < 42.6f;

    auto dstBlob = dst.getBlob(3);
    ok = ok && dstBlob.size() == 2 && dstBlob[0] == std::byte{0xCA};

    if (ok) PASS();
    else FAIL("round-trip mismatch");
}

// Test: variable property callback fires on applyDelta
static void testVarCallbackOnApplyDelta() {
    TEST("variable property callback fires on applyDelta");

    auto def = makeMixedDef();
    Entity entity(1, EntitySide::Base, *def);

    entity.setString(1, "before");

    std::string capturedNew;
    int callCount = 0;

    entity.setPropertyChangedCallback(1,
        [&](Entity&, PropertyId, const std::byte*, const std::byte* newVal, std::size_t size) {
            capturedNew = std::string(reinterpret_cast<const char*>(newVal), size);
            ++callCount;
        });

    PropertyDelta delta;
    delta.propertyId = 1;
    std::string after = "after";
    const auto* afterBegin = reinterpret_cast<const std::byte*>(after.data());
    delta.value.assign(afterBegin, afterBegin + after.size());

    entity.applyPropertyDelta({&delta, 1}, false);

    bool ok = callCount == 1;
    ok = ok && entity.getString(1) == "after";

    if (ok) PASS();
    else FAIL("count=" + std::to_string(callCount) + " val='" + std::string(entity.getString(1)) + "'");
}

// Test: string default value applied on init
static void testStringDefaultValue() {
    TEST("string default value applied on init");

    auto def = std::make_unique<EntityDef>("Test");
    std::string defaultName = "unnamed";
    const auto* nameBegin = reinterpret_cast<const std::byte*>(defaultName.data());
    std::vector<std::byte> defaultBytes(nameBegin, nameBegin + defaultName.size());
    def->addProperty("name", PropertyType::String, 0,
                     theseed::runtime::PropertyFlag::None, std::move(defaultBytes));

    Entity entity(1, EntitySide::Base, *def);
    auto val = entity.getString(0);

    bool ok = val == "unnamed";

    if (ok) PASS();
    else FAIL("expected 'unnamed', got '" + std::string(val) + "'");
}

// Test: long string property
static void testLongString() {
    TEST("long string property (1KB)");

    auto def = makeMixedDef();
    Entity entity(1, EntitySide::Base, *def);

    std::string longStr(1024, 'X');
    entity.setString(1, longStr);
    auto val = entity.getString(1);

    bool ok = val.size() == 1024 && val[0] == 'X' && val[1023] == 'X';

    if (ok) PASS();
    else FAIL("size=" + std::to_string(val.size()));
}

int main() {
    std::cout << "Variable-sized property tests:\n";

    testStringGetSet();
    testStringGetSetByName();
    testStringOverwrite();
    testStringDirtyTracking();
    testStringChangeCallback();
    testBlobGetSet();
    testBlobDirtyTracking();
    testFullSnapshotIncludesStrings();
    testDirtyDeltaIncludesDirtyStrings();
    testApplyDeltaString();
    testApplyDeltaBlob();
    testApplyDeltaMarksDirty();
    testEmptyString();
    testEmptyBlob();
    testFindStringNonExistent();
    testSetStringNonExistent();
    testMixedSnapshotRoundTrip();
    testVarCallbackOnApplyDelta();
    testStringDefaultValue();
    testLongString();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
