#include "theseed/core/FileEntityStore.h"
#include "theseed/core/EntityData.h"
#include "theseed/core/BaseApp.h"
#include "theseed/core/IEntityStore.h"
#include "theseed/runtime/PipedTransport.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

using theseed::core::BaseApp;
using theseed::core::EntityData;
using theseed::core::FileEntityStore;
using theseed::core::InMemoryEntityStore;
using theseed::core::PropertyData;
using theseed::runtime::Entity;
using theseed::runtime::EntityId;

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

static bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << content;
    return true;
}

static std::string createDefDir() {
    std::string dir = "test_restore_defs";
    std::filesystem::create_directory(dir);
    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="level" type="Int32"/>
        <Property name="hp" type="Float32"/>
    </Properties>
</EntityDef>
)");
    return dir;
}

// Test: FileEntityStore::listIdsByType returns stored IDs
static void testListIdsByType() {
    TEST("listIdsByType returns IDs for given entity type");

    auto dir = std::filesystem::temp_directory_path() / "theseed_restore_test_1";
    std::filesystem::remove_all(dir);
    FileEntityStore store(dir);

    EntityData d1, d2, d3;
    d1.id = 1; d1.entityType = "Avatar";
    d2.id = 2; d2.entityType = "Avatar";
    d3.id = 3; d3.entityType = "Npc";

    PropertyData prop;
    prop.id = 0; prop.name = "level"; prop.type = theseed::core::DataType::Int32;
    prop.rawValue.resize(4); std::int32_t v = 5; std::memcpy(prop.rawValue.data(), &v, 4);
    d1.properties.push_back(prop);

    store.save(1, d1);
    store.save(2, d2);
    store.save(3, d3);

    auto avatarIds = store.listIdsByType("Avatar");
    auto npcIds = store.listIdsByType("Npc");
    auto emptyIds = store.listIdsByType("Monster");

    bool ok = avatarIds.size() == 2;
    ok = ok && npcIds.size() == 1;
    ok = ok && emptyIds.empty();
    // Verify IDs are correct (order may vary)
    std::size_t found1 = 0, found2 = 0;
    for (auto id : avatarIds) { if (id == 1) ++found1; if (id == 2) ++found2; }
    ok = ok && found1 == 1 && found2 == 1;
    ok = ok && npcIds[0] == 3;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("avatar=" + std::to_string(avatarIds.size()) + " npc=" + std::to_string(npcIds.size()));
}

// Test: InMemoryEntityStore::listIdsByType
static void testInMemoryListIdsByType() {
    TEST("InMemoryEntityStore listIdsByType works");

    InMemoryEntityStore store;

    EntityData d1, d2;
    d1.id = 10; d1.entityType = "Player";
    d2.id = 20; d2.entityType = "Player";

    store.save(10, d1);
    store.save(20, d2);

    auto ids = store.listIdsByType("Player");
    bool ok = ids.size() == 2;

    auto empty = store.listIdsByType("Enemy");
    ok = ok && empty.empty();

    if (ok) PASS();
    else FAIL("ids=" + std::to_string(ids.size()));
}

// Test: BaseRuntime::restoreEntities restores all entities of a type
static void testRestoreEntities() {
    TEST("BaseRuntime::restoreEntities restores saved entities");

    auto dir = std::filesystem::temp_directory_path() / "theseed_restore_test_2";
    std::filesystem::remove_all(dir);
    auto defDir = createDefDir();

    auto transport = std::make_shared<theseed::runtime::PipedTransport>(1);
    auto store = std::make_shared<FileEntityStore>(dir);

    // Phase 1: Create and save entities
    {
        BaseApp app(BaseApp::Config{.entityDefPath = defDir, .componentId = 1}, transport, store);
        app.init();

        auto* e1 = app.createEntity("Avatar");
        auto* e2 = app.createEntity("Avatar");
        e1->setProperty<std::int32_t>(0, 10);  // level = 10
        e1->setProperty<float>(1, 80.0f);       // hp = 80
        e2->setProperty<std::int32_t>(0, 25);  // level = 25
        e2->setProperty<float>(1, 200.0f);      // hp = 200

        app.runtime().saveEntity(e1->id());
        app.runtime().saveEntity(e2->id());
    }

    // Phase 2: Auto-restore entities in a fresh BaseApp
    {
        auto transport2 = std::make_shared<theseed::runtime::PipedTransport>(1);
        BaseApp app(BaseApp::Config{.entityDefPath = defDir, .componentId = 1}, transport2, store);
        app.init();

        // Auto-restore happens in init(), entities should already be loaded
        bool ok = app.runtime().entityCount() == 2;

        // Find and verify entities
        auto ids = store->listIdsByType("Avatar");
        ok = ok && ids.size() == 2;

        for (auto id : ids) {
            auto* entity = app.findEntity(id);
            ok = ok && entity != nullptr;
            if (!entity) continue;
            ok = ok && entity->entityType() == "Avatar";

            auto level = entity->getProperty<std::int32_t>(0);
            auto hp = entity->getProperty<float>(1);
            bool isE1 = (level == 10 && std::abs(hp - 80.0f) < 0.01f);
            bool isE2 = (level == 25 && std::abs(hp - 200.0f) < 0.01f);
            ok = ok && (isE1 || isE2);
        }

        std::filesystem::remove_all(dir);
        std::filesystem::remove_all(defDir);
        if (ok) PASS();
        else FAIL("entityCount=" + std::to_string(app.runtime().entityCount()));
    }
}

// Test: restoreEntities skips already-loaded entities
static void testRestoreSkipsExisting() {
    TEST("restoreEntities skips already-loaded entities");

    auto dir = std::filesystem::temp_directory_path() / "theseed_restore_test_3";
    std::filesystem::remove_all(dir);
    auto defDir = createDefDir();

    auto transport = std::make_shared<theseed::runtime::PipedTransport>(1);
    auto store = std::make_shared<FileEntityStore>(dir);

    BaseApp app(BaseApp::Config{.entityDefPath = defDir, .componentId = 1}, transport, store);
    app.init();

    auto* entity = app.createEntity("Avatar");
    entity->setProperty<std::int32_t>(0, 5);
    app.runtime().saveEntity(entity->id());

    auto count = app.restoreEntities("Avatar");
    bool ok = count == 0;  // Already loaded, skip
    ok = ok && app.runtime().entityCount() == 1;

    std::filesystem::remove_all(dir);
    std::filesystem::remove_all(defDir);
    if (ok) PASS();
    else FAIL("count=" + std::to_string(count));
}

// Test: restoreEntities returns 0 for unknown type
static void testRestoreUnknownType() {
    TEST("restoreEntities returns 0 for unknown type");

    auto dir = std::filesystem::temp_directory_path() / "theseed_restore_test_4";
    std::filesystem::remove_all(dir);

    auto transport = std::make_shared<theseed::runtime::PipedTransport>(1);
    auto store = std::make_shared<FileEntityStore>(dir);

    // No def path, no factories registered
    BaseApp app(BaseApp::Config{.componentId = 1}, transport, store);
    app.init();

    auto count = app.restoreEntities("NonExistent");
    bool ok = count == 0;

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("count=" + std::to_string(count));
}

int main() {
    std::cout << "Entity restore tests:\n";

    testListIdsByType();
    testInMemoryListIdsByType();
    testRestoreEntities();
    testRestoreSkipsExisting();
    testRestoreUnknownType();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
