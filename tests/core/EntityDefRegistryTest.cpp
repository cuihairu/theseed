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

// --- Inheritance tests ---

static void testInheritanceFromFiles() {
    TEST("inheritance: child gets parent properties and methods");

    std::string dir = "test_registry_inherit";
    std::filesystem::create_directory(dir);

    writeFile(dir + "/Base.xml", R"(
<EntityDef name="Base">
    <Properties>
        <Property name="hp" type="Int32" defaultValue="100"/>
        <Property name="level" type="Int32"/>
    </Properties>
    <Methods>
        <Method name="onDamage" side="Cell"/>
    </Methods>
</EntityDef>
)");

    writeFile(dir + "/Child.xml", R"(
<EntityDef name="Child" extends="Base">
    <Properties>
        <Property name="mana" type="Int32"/>
    </Properties>
    <Methods>
        <Method name="onCast" side="Base"/>
    </Methods>
</EntityDef>
)");

    EntityDefRegistry registry;
    auto count = registry.loadDirectory(dir);

    bool ok = count == 2;
    auto& child = registry.getDef("Child");
    ok = ok && child != nullptr;
    ok = ok && child->propertyCount() == 3;  // hp + level + mana
    ok = ok && child->methodCount() == 2;    // onDamage + onCast
    ok = ok && child->findProperty("hp") != nullptr;
    ok = ok && child->findProperty("mana") != nullptr;
    ok = ok && child->findMethod("onDamage") != nullptr;
    ok = ok && child->findMethod("onCast") != nullptr;

    std::filesystem::remove_all(dir);
    if (ok) PASS(); else FAIL("props=" + std::to_string(child->propertyCount())
                               + " methods=" + std::to_string(child->methodCount()));
}

static void testInheritanceMultiLevel() {
    TEST("inheritance: A -> B -> C chain resolves correctly");

    std::string dir = "test_registry_multi_inherit";
    std::filesystem::create_directory(dir);

    writeFile(dir + "/Root.xml", R"(
<EntityDef name="Root">
    <Properties><Property name="id" type="Int32"/></Properties>
</EntityDef>
)");
    writeFile(dir + "/Mid.xml", R"(
<EntityDef name="Mid" extends="Root">
    <Properties><Property name="hp" type="Int32"/></Properties>
</EntityDef>
)");
    writeFile(dir + "/Leaf.xml", R"(
<EntityDef name="Leaf" extends="Mid">
    <Properties><Property name="name" type="String"/></Properties>
</EntityDef>
)");

    EntityDefRegistry registry;
    registry.loadDirectory(dir);

    auto& leaf = registry.getDef("Leaf");
    bool ok = leaf != nullptr;
    ok = ok && leaf->propertyCount() == 3;
    ok = ok && leaf->findProperty("id") != nullptr;
    ok = ok && leaf->findProperty("hp") != nullptr;
    ok = ok && leaf->findProperty("name") != nullptr;

    // Mid should also have Root's properties
    auto& mid = registry.getDef("Mid");
    ok = ok && mid != nullptr;
    ok = ok && mid->propertyCount() == 2;

    std::filesystem::remove_all(dir);
    if (ok) PASS(); else FAIL("leaf props=" + std::to_string(leaf->propertyCount()));
}

static void testInheritedDefCreatesEntity() {
    TEST("inheritance: factory from inherited def creates valid entity");

    std::string dir = "test_registry_inherit_factory";
    std::filesystem::create_directory(dir);

    writeFile(dir + "/Base.xml", R"(
<EntityDef name="Base">
    <Properties>
        <Property name="hp" type="Int32" defaultValue="100"/>
        <Property name="speed" type="Float32"/>
    </Properties>
</EntityDef>
)");
    writeFile(dir + "/Child.xml", R"(
<EntityDef name="Child" extends="Base">
    <Properties>
        <Property name="mana" type="Int32" defaultValue="50"/>
    </Properties>
</EntityDef>
)");

    EntityDefRegistry registry;
    registry.loadDirectory(dir);

    auto factory = registry.createFactory("Child");
    bool ok = static_cast<bool>(factory);

    auto entity = factory(1, EntitySide::Cell);
    ok = ok && entity != nullptr;
    ok = ok && entity->entityType() == "Child";

    // Verify all properties accessible (hp=0, speed=0, mana=50)
    auto* hpDesc = registry.getDef("Child")->findProperty("hp");
    auto* manaDesc = registry.getDef("Child")->findProperty("mana");
    ok = ok && hpDesc != nullptr && manaDesc != nullptr;

    if (ok) {
        auto hp = entity->getProperty<std::int32_t>(hpDesc->id);
        auto mana = entity->getProperty<std::int32_t>(manaDesc->id);
        ok = ok && hp == 100;
        ok = ok && mana == 50;
    }

    std::filesystem::remove_all(dir);
    if (ok) PASS(); else FAIL("entity creation from inherited def failed");
}

static void testMissingParentHandled() {
    TEST("inheritance: missing parent does not crash");

    std::string dir = "test_registry_missing_parent";
    std::filesystem::create_directory(dir);

    writeFile(dir + "/Orphan.xml", R"(
<EntityDef name="Orphan" extends="NonExistent">
    <Properties><Property name="x" type="Int32"/></Properties>
</EntityDef>
)");

    EntityDefRegistry registry;
    auto count = registry.loadDirectory(dir);

    bool ok = count == 1;
    auto& orphan = registry.getDef("Orphan");
    ok = ok && orphan != nullptr;
    ok = ok && orphan->parentType() == "NonExistent";
    // Only own properties, no merge happened
    ok = ok && orphan->propertyCount() == 1;

    std::filesystem::remove_all(dir);
    if (ok) PASS(); else FAIL("props=" + std::to_string(orphan->propertyCount()));
}

static void testInheritanceWithFlags() {
    TEST("inheritance: property flags preserved from parent");

    std::string dir = "test_registry_inherit_flags";
    std::filesystem::create_directory(dir);

    writeFile(dir + "/Base.xml", R"(
<EntityDef name="Base">
    <Properties>
        <Property name="saveMe" type="Int32" flags="Persistent"/>
        <Property name="cellOnly" type="Float32" flags="Cell"/>
    </Properties>
</EntityDef>
)");
    writeFile(dir + "/Child.xml", R"(
<EntityDef name="Child" extends="Base">
    <Properties>
        <Property name="ownProp" type="Int32" flags="Base"/>
    </Properties>
</EntityDef>
)");

    EntityDefRegistry registry;
    registry.loadDirectory(dir);

    auto& child = registry.getDef("Child");
    bool ok = child != nullptr;

    auto* saveMe = child->findProperty("saveMe");
    auto* cellOnly = child->findProperty("cellOnly");
    auto* ownProp = child->findProperty("ownProp");

    ok = ok && saveMe != nullptr;
    ok = ok && cellOnly != nullptr;
    ok = ok && ownProp != nullptr;

    using PF = theseed::runtime::PropertyFlag;
    ok = ok && theseed::runtime::hasFlag(saveMe->flags, PF::Persistent);
    ok = ok && theseed::runtime::hasFlag(cellOnly->flags, PF::Cell);
    ok = ok && theseed::runtime::hasFlag(ownProp->flags, PF::Base);

    std::filesystem::remove_all(dir);
    if (ok) PASS(); else FAIL("flag check failed");
}

static void testMergeFromIsIdempotent() {
    TEST("inheritance: mergeFrom called twice is no-op");

    EntityDef parent("Parent");
    parent.addProperty("hp", PropertyType::Int32);

    EntityDef child("Child");
    child.setParentType("Parent");

    bool first = child.mergeFrom(parent);
    bool second = child.mergeFrom(parent);

    bool ok = first == true && second == false;
    ok = ok && child.propertyCount() == 1;

    if (ok) PASS(); else FAIL("first=" + std::to_string(first) + " second=" + std::to_string(second));
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

    // Inheritance tests
    testInheritanceFromFiles();
    testInheritanceMultiLevel();
    testInheritedDefCreatesEntity();
    testMissingParentHandled();
    testInheritanceWithFlags();
    testMergeFromIsIdempotent();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
