#include "theseed/core/EntityDefRegistry.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

using theseed::core::EntityDefRegistry;
using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntitySide;
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

static bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << content;
    return true;
}

static void testRegisterDef() {
    TEST("register and retrieve definition");

    EntityDefRegistry registry;
    auto def = std::make_shared<EntityDef>("Avatar");
    def->addProperty("level", PropertyType::Int32);

    bool ok = registry.registerDef(def);
    ok = ok && registry.hasDef("Avatar");
    ok = ok && registry.getDef("Avatar") != nullptr;
    ok = ok && registry.getDef("Avatar")->entityType() == "Avatar";
    ok = ok && registry.defCount() == 1;

    if (ok) PASS(); else FAIL("register def failed");
}

static void testLoadFile() {
    TEST("load definition from XML file");

    const char* path = "test_registry_entity.xml";
    writeFile(path, R"(
<EntityDef name="Monster">
    <Properties>
        <Property name="hp" type="Int32"/>
        <Property name="speed" type="Float32"/>
    </Properties>
</EntityDef>
)");

    EntityDefRegistry registry;
    bool ok = registry.loadFile(path);
    ok = ok && registry.hasDef("Monster");
    ok = ok && registry.getDef("Monster")->propertyCount() == 2;

    std::filesystem::remove(path);

    if (ok) PASS(); else FAIL("load file failed");
}

static void testLoadDirectory() {
    TEST("load definitions from directory");

    std::string dir = "test_registry_dir";
    std::filesystem::create_directory(dir);

    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="level" type="Int32"/>
    </Properties>
</EntityDef>
)");

    writeFile(dir + "/Monster.xml", R"(
<EntityDef name="Monster">
    <Properties>
        <Property name="hp" type="Int32"/>
    </Properties>
</EntityDef>
)");

    writeFile(dir + "/readme.txt", "not a def file");

    EntityDefRegistry registry;
    auto count = registry.loadDirectory(dir);

    bool ok = count == 2;
    ok = ok && registry.hasDef("Avatar");
    ok = ok && registry.hasDef("Monster");
    ok = ok && registry.defCount() == 2;

    std::filesystem::remove_all(dir);

    if (ok) PASS(); else FAIL("load directory failed, count=" + std::to_string(count));
}

static void testCreateFactory() {
    TEST("create factory and build entity");

    EntityDefRegistry registry;
    auto def = std::make_shared<EntityDef>("Avatar");
    def->addProperty("level", PropertyType::Int32);
    def->addProperty("hp", PropertyType::Float32);
    registry.registerDef(def);

    auto factory = registry.createFactory("Avatar");
    bool ok = static_cast<bool>(factory);

    auto entity = factory(1, EntitySide::Base);
    ok = ok && entity != nullptr;
    ok = ok && entity->entityType() == "Avatar";
    ok = ok && entity->side() == EntitySide::Base;
    ok = ok && entity->id() == 1;

    entity->setProperty<std::int32_t>(0, 42);
    ok = ok && entity->getProperty<std::int32_t>(0) == 42;

    if (ok) PASS(); else FAIL("create factory failed");
}

static void testEntityTypes() {
    TEST("list entity types");

    EntityDefRegistry registry;
    registry.registerDef(std::make_shared<EntityDef>("Avatar"));
    registry.registerDef(std::make_shared<EntityDef>("Monster"));
    registry.registerDef(std::make_shared<EntityDef>("Npc"));

    auto types = registry.entityTypes();
    bool ok = types.size() == 3;

    std::sort(types.begin(), types.end());
    ok = ok && types[0] == "Avatar";
    ok = ok && types[1] == "Monster";
    ok = ok && types[2] == "Npc";

    if (ok) PASS(); else FAIL("entity types list failed");
}

static void testDuplicateRegister() {
    TEST("duplicate register overwrites");

    EntityDefRegistry registry;

    auto def1 = std::make_shared<EntityDef>("Avatar");
    def1->addProperty("level", PropertyType::Int32);
    registry.registerDef(def1);

    auto def2 = std::make_shared<EntityDef>("Avatar");
    def2->addProperty("level", PropertyType::Int32);
    def2->addProperty("name", PropertyType::String);
    registry.registerDef(def2);

    bool ok = registry.defCount() == 1;
    ok = ok && registry.getDef("Avatar")->propertyCount() == 2;

    if (ok) PASS(); else FAIL("duplicate register failed");
}

static void testGetNonexistent() {
    TEST("get nonexistent returns null");

    EntityDefRegistry registry;
    bool ok = !registry.hasDef("NotExist");
    ok = ok && registry.getDef("NotExist") == nullptr;
    ok = ok && registry.createFactory("NotExist") == nullptr;

    if (ok) PASS(); else FAIL("nonexistent check failed");
}

int main() {
    std::cout << "EntityDefRegistry tests:\n";

    testRegisterDef();
    testLoadFile();
    testLoadDirectory();
    testCreateFactory();
    testEntityTypes();
    testDuplicateRegister();
    testGetNonexistent();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
