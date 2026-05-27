#include "theseed/core/FileEntityStore.h"
#include "theseed/core/EntityData.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

using theseed::core::EntityData;
using theseed::core::FileEntityStore;
using theseed::core::PropertyData;

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

static void fillInt32Property(PropertyData& prop, std::uint32_t id,
                               const std::string& name, std::int32_t value) {
    prop.id = id;
    prop.name = name;
    prop.type = theseed::core::DataType::Int32;
    prop.rawValue.resize(sizeof(value));
    std::memcpy(prop.rawValue.data(), &value, sizeof(value));
}

static void fillFloat32Property(PropertyData& prop, std::uint32_t id,
                                 const std::string& name, float value) {
    prop.id = id;
    prop.name = name;
    prop.type = theseed::core::DataType::Float32;
    prop.rawValue.resize(sizeof(value));
    std::memcpy(prop.rawValue.data(), &value, sizeof(value));
}

static std::int32_t readInt32(const PropertyData& prop) {
    std::int32_t v = 0;
    std::memcpy(&v, prop.rawValue.data(), sizeof(v));
    return v;
}

static float readFloat32(const PropertyData& prop) {
    float v = 0;
    std::memcpy(&v, prop.rawValue.data(), sizeof(v));
    return v;
}

// Test: basic save and load round-trip
static void testSaveAndLoad() {
    TEST("save and load round-trip");

    auto dir = std::filesystem::temp_directory_path() / "theseed_file_store_test_1";
    std::filesystem::remove_all(dir);
    FileEntityStore store(dir);

    EntityData original;
    original.id = 42;
    original.entityType = "Avatar";
    PropertyData levelProp, hpProp;
    fillInt32Property(levelProp, 0, "level", 10);
    fillFloat32Property(hpProp, 1, "hp", 85.5f);
    original.properties.push_back(levelProp);
    original.properties.push_back(hpProp);

    bool ok = store.save(42, original);
    ok = ok && store.exists(42, "Avatar");

    EntityData loaded;
    ok = ok && store.load(42, "Avatar", loaded);
    ok = ok && loaded.id == 42;
    ok = ok && loaded.entityType == "Avatar";
    ok = ok && loaded.properties.size() == 2;
    if (ok) {
        ok = ok && readInt32(loaded.properties[0]) == 10;
        ok = ok && std::abs(readFloat32(loaded.properties[1]) - 85.5f) < 0.001f;
    }

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("data mismatch");
}

// Test: load non-existent entity returns false
static void testLoadNonExistent() {
    TEST("load non-existent entity returns false");

    auto dir = std::filesystem::temp_directory_path() / "theseed_file_store_test_2";
    std::filesystem::remove_all(dir);
    FileEntityStore store(dir);

    EntityData out;
    bool ok = !store.load(999, "Avatar", out);
    ok = ok && !store.exists(999, "Avatar");

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("should return false");
}

// Test: remove entity deletes file
static void testRemoveEntity() {
    TEST("remove entity deletes file");

    auto dir = std::filesystem::temp_directory_path() / "theseed_file_store_test_3";
    std::filesystem::remove_all(dir);
    FileEntityStore store(dir);

    EntityData data;
    data.id = 7;
    data.entityType = "Npc";
    PropertyData prop;
    fillInt32Property(prop, 0, "kind", 3);
    data.properties.push_back(prop);

    bool ok = store.save(7, data);
    ok = ok && store.exists(7, "Npc");
    ok = ok && store.remove(7);
    ok = ok && !store.exists(7, "Npc");

    EntityData out;
    ok = ok && !store.load(7, "Npc", out);

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("remove failed");
}

// Test: allocId returns sequential IDs
static void testSequentialIdAllocation() {
    TEST("allocId returns sequential IDs");

    auto dir = std::filesystem::temp_directory_path() / "theseed_file_store_test_4";
    std::filesystem::remove_all(dir);
    FileEntityStore store(dir);

    bool ok = store.allocId() == 1;
    ok = ok && store.allocId() == 2;
    ok = ok && store.allocId() == 3;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("IDs not sequential");
}

// Test: allocId persists across store instances
static void testIdPersistenceAcrossInstances() {
    TEST("allocId persists across store instances");

    auto dir = std::filesystem::temp_directory_path() / "theseed_file_store_test_5";
    std::filesystem::remove_all(dir);

    {
        FileEntityStore store(dir);
        store.allocId();  // 1
        store.allocId();  // 2
        store.allocId();  // 3
    }

    {
        FileEntityStore store(dir);
        bool ok = store.allocId() == 4;
        ok = ok && store.allocId() == 5;

        std::filesystem::remove_all(dir);
        if (ok) PASS();
        else FAIL("ID counter not persisted");
    }
}

// Test: multiple entity types stored in separate directories
static void testMultipleEntityTypes() {
    TEST("multiple entity types in separate directories");

    auto dir = std::filesystem::temp_directory_path() / "theseed_file_store_test_6";
    std::filesystem::remove_all(dir);
    FileEntityStore store(dir);

    EntityData avatar;
    avatar.id = 1;
    avatar.entityType = "Avatar";
    PropertyData prop;
    fillInt32Property(prop, 0, "level", 5);
    avatar.properties.push_back(prop);

    EntityData npc;
    npc.id = 2;
    npc.entityType = "Npc";
    PropertyData npcProp;
    fillInt32Property(npcProp, 0, "kind", 10);
    npc.properties.push_back(npcProp);

    bool ok = store.save(1, avatar);
    ok = ok && store.save(2, npc);
    ok = ok && store.exists(1, "Avatar");
    ok = ok && store.exists(2, "Npc");

    EntityData loadedAvatar, loadedNpc;
    ok = ok && store.load(1, "Avatar", loadedAvatar);
    ok = ok && store.load(2, "Npc", loadedNpc);
    ok = ok && readInt32(loadedAvatar.properties[0]) == 5;
    ok = ok && readInt32(loadedNpc.properties[0]) == 10;

    // Loading with wrong type should fail
    ok = ok && !store.load(1, "Npc", loadedAvatar);

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("multi-type storage failed");
}

// Test: overwrite existing entity
static void testOverwriteEntity() {
    TEST("overwrite existing entity with new data");

    auto dir = std::filesystem::temp_directory_path() / "theseed_file_store_test_7";
    std::filesystem::remove_all(dir);
    FileEntityStore store(dir);

    EntityData data;
    data.id = 1;
    data.entityType = "Avatar";
    PropertyData prop;
    fillInt32Property(prop, 0, "level", 1);
    data.properties.push_back(prop);
    store.save(1, data);

    // Overwrite with new level
    data.properties.clear();
    fillInt32Property(prop, 0, "level", 50);
    data.properties.push_back(prop);
    bool ok = store.save(1, data);

    EntityData loaded;
    ok = ok && store.load(1, "Avatar", loaded);
    ok = ok && readInt32(loaded.properties[0]) == 50;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("overwrite failed");
}

// Test: save entity with no properties
static void testSaveEmptyProperties() {
    TEST("save and load entity with no properties");

    auto dir = std::filesystem::temp_directory_path() / "theseed_file_store_test_8";
    std::filesystem::remove_all(dir);
    FileEntityStore store(dir);

    EntityData data;
    data.id = 1;
    data.entityType = "Empty";

    bool ok = store.save(1, data);
    EntityData loaded;
    ok = ok && store.load(1, "Empty", loaded);
    ok = ok && loaded.id == 1;
    ok = ok && loaded.entityType == "Empty";
    ok = ok && loaded.properties.empty();

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("empty properties failed");
}

// Test: integration with BaseRuntime save/load cycle
static void testBaseRuntimeIntegration() {
    TEST("BaseRuntime integration: create→save→destroy→reload");

    auto dir = std::filesystem::temp_directory_path() / "theseed_file_store_test_9";
    std::filesystem::remove_all(dir);

    // We can't directly test BaseRuntime here without linking to the full core library
    // and setting up transports. Instead, verify FileEntityStore works with the same
    // data format that BaseRuntime uses.
    FileEntityStore store(dir);

    // Simulate what BaseRuntime::saveEntity does
    EntityData data;
    data.id = store.allocId();
    data.entityType = "Avatar";
    PropertyData levelProp, hpProp;
    fillInt32Property(levelProp, 0, "level", 25);
    fillFloat32Property(hpProp, 1, "hp", 120.0f);
    data.properties.push_back(levelProp);
    data.properties.push_back(hpProp);

    bool ok = store.save(data.id, data);

    // Simulate what BaseRuntime::loadEntity does
    EntityData loaded;
    ok = ok && store.load(data.id, "Avatar", loaded);
    ok = ok && loaded.id == data.id;
    ok = ok && loaded.entityType == "Avatar";
    ok = ok && loaded.properties.size() == 2;
    if (ok) {
        ok = ok && readInt32(loaded.properties[0]) == 25;
        ok = ok && std::abs(readFloat32(loaded.properties[1]) - 120.0f) < 0.001f;
    }

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("integration simulation failed");
}

int main() {
    std::cout << "File entity store tests:\n";

    testSaveAndLoad();
    testLoadNonExistent();
    testRemoveEntity();
    testSequentialIdAllocation();
    testIdPersistenceAcrossInstances();
    testMultipleEntityTypes();
    testOverwriteEntity();
    testSaveEmptyProperties();
    testBaseRuntimeIntegration();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
