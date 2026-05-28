#include "theseed/core/EntityDefLoader.h"
#include "theseed/core/EntityDefRegistry.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

using theseed::core::EntityDefLoader;
using theseed::core::EntityDefRegistry;
using theseed::runtime::EntityDef;
using theseed::runtime::MethodSide;
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

static void testLoadBasicEntity() {
    TEST("load basic entity with properties");

    const char* xml = R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="level" type="Int32"/>
        <Property name="name" type="String"/>
        <Property name="hp" type="Float32"/>
        <Property name="alive" type="Bool"/>
    </Properties>
</EntityDef>
)";

    auto def = EntityDefLoader::loadFromString(xml);

    bool ok = def != nullptr;
    ok = ok && def->entityType() == "Avatar";
    ok = ok && def->propertyCount() == 4;

    ok = ok && def->findProperty("level")->type == PropertyType::Int32;
    ok = ok && def->findProperty("name")->type == PropertyType::String;
    ok = ok && def->findProperty("hp")->type == PropertyType::Float32;
    ok = ok && def->findProperty("alive")->type == PropertyType::Bool;

    if (ok) PASS(); else FAIL("basic entity load failed");
}

static void testLoadWithMethods() {
    TEST("load entity with methods");

    const char* xml = R"(
<EntityDef name="Monster">
    <Properties>
        <Property name="hp" type="Int32"/>
    </Properties>
    <Methods>
        <Method name="onDamage" side="Cell"/>
        <Method name="onDie" side="Base"/>
    </Methods>
</EntityDef>
)";

    auto def = EntityDefLoader::loadFromString(xml);

    bool ok = def != nullptr;
    ok = ok && def->propertyCount() == 1;
    ok = ok && def->methodCount() == 2;

    ok = ok && def->findMethod("onDamage") != nullptr;
    ok = ok && def->findMethod("onDamage")->side == MethodSide::Cell;
    ok = ok && def->findMethod("onDie") != nullptr;
    ok = ok && def->findMethod("onDie")->side == MethodSide::Base;

    if (ok) PASS(); else FAIL("methods load failed");
}

static void testLoadAllTypes() {
    TEST("load all property types");

    const char* xml = R"(
<EntityDef name="AllTypes">
    <Properties>
        <Property name="a" type="Int8"/>
        <Property name="b" type="Int16"/>
        <Property name="c" type="Int32"/>
        <Property name="d" type="Int64"/>
        <Property name="e" type="UInt8"/>
        <Property name="f" type="UInt16"/>
        <Property name="g" type="UInt32"/>
        <Property name="h" type="UInt64"/>
        <Property name="i" type="Float32"/>
        <Property name="j" type="Float64"/>
        <Property name="k" type="Bool"/>
        <Property name="l" type="String"/>
        <Property name="m" type="Vector3"/>
        <Property name="n" type="Blob"/>
    </Properties>
</EntityDef>
)";

    auto def = EntityDefLoader::loadFromString(xml);

    bool ok = def != nullptr && def->propertyCount() == 14;

    ok = ok && def->findProperty("a")->type == PropertyType::Int8;
    ok = ok && def->findProperty("d")->type == PropertyType::Int64;
    ok = ok && def->findProperty("h")->type == PropertyType::UInt64;
    ok = ok && def->findProperty("l")->type == PropertyType::String;
    ok = ok && def->findProperty("m")->type == PropertyType::Vector3;
    ok = ok && def->findProperty("n")->type == PropertyType::Blob;

    if (ok) PASS(); else FAIL("all types load failed");
}

static void testLoadFixedSize() {
    TEST("fixed size type storage calculation");

    const char* xml = R"(
<EntityDef name="Sizes">
    <Properties>
        <Property name="x" type="Int32"/>
        <Property name="y" type="Float32"/>
        <Property name="pos" type="Vector3"/>
    </Properties>
</EntityDef>
)";

    auto def = EntityDefLoader::loadFromString(xml);

    // Int32(4) + Float32(4) + Vector3(12) = 20
    bool ok = def->storageSize() == 20;

    if (ok) PASS(); else FAIL("expected storageSize=20, got " + std::to_string(def->storageSize()));
}

static void testLoadEmptyEntity() {
    TEST("load empty entity");

    const char* xml = R"(<EntityDef name="Empty"/>)";

    auto def = EntityDefLoader::loadFromString(xml);

    bool ok = def != nullptr;
    ok = ok && def->entityType() == "Empty";
    ok = ok && def->propertyCount() == 0;
    ok = ok && def->methodCount() == 0;

    if (ok) PASS(); else FAIL("empty entity failed");
}

static void testLoadInvalidType() {
    TEST("load with invalid type throws");

    const char* xml = R"(
<EntityDef name="Bad">
    <Properties>
        <Property name="x" type="InvalidType"/>
    </Properties>
</EntityDef>
)";

    bool threw = false;
    try {
        EntityDefLoader::loadFromString(xml);
    } catch (const std::runtime_error&) {
        threw = true;
    }

    if (threw) PASS(); else FAIL("expected exception");
}

static void testLoadMethodWithArgs() {
    TEST("parse method with Arg elements");

    EntityDefLoader loader;
    auto def = loader.loadFromString(R"(
<EntityDef name="Player">
    <Properties>
        <Property name="hp" type="Int32"/>
    </Properties>
    <Methods>
        <Method name="onDamage" side="Cell">
            <Arg name="amount" type="Int32"/>
            <Arg name="source" type="UInt64"/>
        </Method>
        <Method name="onHeal" side="Base">
            <Arg name="hp" type="Float32"/>
        </Method>
        <Method name="respawn" side="Cell"/>
    </Methods>
</EntityDef>
)");

    bool ok = def->methodCount() == 3;

    auto* damage = def->findMethod("onDamage");
    ok = ok && damage != nullptr;
    ok = ok && damage->side == MethodSide::Cell;
    ok = ok && damage->args.size() == 2;
    ok = ok && damage->args[0].name == "amount";
    ok = ok && damage->args[0].type == PropertyType::Int32;
    ok = ok && damage->args[1].name == "source";
    ok = ok && damage->args[1].type == PropertyType::UInt64;

    auto* heal = def->findMethod("onHeal");
    ok = ok && heal != nullptr;
    ok = ok && heal->side == MethodSide::Base;
    ok = ok && heal->args.size() == 1;
    ok = ok && heal->args[0].name == "hp";
    ok = ok && heal->args[0].type == PropertyType::Float32;

    auto* respawn = def->findMethod("respawn");
    ok = ok && respawn != nullptr;
    ok = ok && respawn->args.empty();

    if (ok) PASS(); else FAIL("method args parsing failed");
}

static bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << content;
    return true;
}

static void testInheritance() {
    TEST("EntityDef inherits from parent via extends");

    auto dir = std::filesystem::temp_directory_path() / "theseed_inherit_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    writeFile((dir / "Creature.xml").string(), R"(
<EntityDef name="Creature">
    <Properties>
        <Property name="hp" type="Int32"/>
        <Property name="level" type="Int32"/>
    </Properties>
    <Methods>
        <Method name="onDamage" side="Cell"/>
    </Methods>
</EntityDef>
)");

    writeFile((dir / "Player.xml").string(), R"(
<EntityDef name="Player" extends="Creature">
    <Properties>
        <Property name="name" type="String"/>
    </Properties>
    <Methods>
        <Method name="onChat" side="Base"/>
    </Methods>
</EntityDef>
)");

    EntityDefRegistry registry;
    auto loaded = registry.loadDirectory(dir.string());

    bool ok = loaded == 2;
    ok = ok && registry.hasDef("Creature");
    ok = ok && registry.hasDef("Player");

    auto& player = registry.getDef("Player");
    ok = ok && player != nullptr;
    ok = ok && player->propertyCount() == 3;  // hp + level + name
    ok = ok && player->methodCount() == 2;    // onDamage + onChat

    ok = ok && player->findProperty("hp") != nullptr;
    ok = ok && player->findProperty("level") != nullptr;
    ok = ok && player->findProperty("name") != nullptr;
    ok = ok && player->findMethod("onDamage") != nullptr;
    ok = ok && player->findMethod("onChat") != nullptr;

    // Verify inherited properties have correct sizes
    auto* hp = player->findProperty("hp");
    auto* level = player->findProperty("level");
    ok = ok && hp != nullptr && hp->size == 4;
    ok = ok && level != nullptr && level->size == 4;

    std::filesystem::remove_all(dir);
    if (ok) PASS(); else FAIL("inheritance failed, props=" + std::to_string(player->propertyCount())
                               + " methods=" + std::to_string(player->methodCount()));
}

static void testMultiLevelInheritance() {
    TEST("multi-level inheritance: A -> B -> C");

    auto dir = std::filesystem::temp_directory_path() / "theseed_multi_inherit_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    writeFile((dir / "Base.xml").string(), R"(
<EntityDef name="Base">
    <Properties><Property name="id" type="Int32"/></Properties>
</EntityDef>
)");
    writeFile((dir / "Mid.xml").string(), R"(
<EntityDef name="Mid" extends="Base">
    <Properties><Property name="hp" type="Int32"/></Properties>
</EntityDef>
)");
    writeFile((dir / "Leaf.xml").string(), R"(
<EntityDef name="Leaf" extends="Mid">
    <Properties><Property name="name" type="String"/></Properties>
</EntityDef>
)");

    EntityDefRegistry registry;
    registry.loadDirectory(dir.string());

    auto& leaf = registry.getDef("Leaf");
    bool ok = leaf != nullptr;
    ok = ok && leaf->propertyCount() == 3;  // id + hp + name
    ok = ok && leaf->findProperty("id") != nullptr;
    ok = ok && leaf->findProperty("hp") != nullptr;
    ok = ok && leaf->findProperty("name") != nullptr;

    std::filesystem::remove_all(dir);
    if (ok) PASS(); else FAIL("props=" + std::to_string(leaf->propertyCount()));
}

static void testDuplicatePropertyRejected() {
    TEST("duplicate property name throws");

    EntityDef def("Test");
    def.addProperty("hp", PropertyType::Int32);

    bool threw = false;
    try {
        def.addProperty("hp", PropertyType::Float32);
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    if (threw) PASS(); else FAIL("expected exception");
}

static void testEmptyPropertyRejected() {
    TEST("empty property name throws");

    EntityDef def("Test");
    bool threw = false;
    try {
        def.addProperty("", PropertyType::Int32);
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    if (threw) PASS(); else FAIL("expected exception");
}

// Test: String property default value from XML
static void testStringDefaultValue() {
    TEST("String property default value from XML");

    const char* xml = R"(
<EntityDef name="Item">
    <Properties>
        <Property name="name" type="String" defaultValue="unnamed"/>
        <Property name="count" type="Int32" defaultValue="1"/>
    </Properties>
</EntityDef>
)";

    auto def = EntityDefLoader::loadFromString(xml);
    const auto* nameProp = def->findProperty("name");
    const auto* countProp = def->findProperty("count");

    bool ok = nameProp != nullptr && countProp != nullptr;
    ok = ok && !nameProp->defaultValue.empty();
    ok = ok && std::string(reinterpret_cast<const char*>(nameProp->defaultValue.data()),
                            nameProp->defaultValue.size()) == "unnamed";
    ok = ok && countProp->defaultValue.size() == 4;

    std::int32_t countVal = 0;
    std::memcpy(&countVal, countProp->defaultValue.data(), 4);
    ok = ok && countVal == 1;

    if (ok) PASS();
    else FAIL("name size=" + std::to_string(nameProp->defaultValue.size())
              + " count=" + std::to_string(countVal));
}

// Test: Vector3 property default value from XML
static void testVector3DefaultValue() {
    TEST("Vector3 property default value from XML");

    const char* xml = R"(
<EntityDef name="Player">
    <Properties>
        <Property name="spawnPos" type="Vector3" defaultValue="10.5,20,30"/>
    </Properties>
</EntityDef>
)";

    auto def = EntityDefLoader::loadFromString(xml);
    const auto* posProp = def->findProperty("spawnPos");

    bool ok = posProp != nullptr;
    ok = ok && posProp->defaultValue.size() == sizeof(float) * 3;

    float vals[3] = {0, 0, 0};
    if (posProp->defaultValue.size() == sizeof(float) * 3) {
        std::memcpy(vals, posProp->defaultValue.data(), sizeof(float) * 3);
    }
    ok = ok && std::abs(vals[0] - 10.5f) < 0.01f;
    ok = ok && std::abs(vals[1] - 20.0f) < 0.01f;
    ok = ok && std::abs(vals[2] - 30.0f) < 0.01f;

    if (ok) PASS();
    else FAIL("x=" + std::to_string(vals[0]) + " y=" + std::to_string(vals[1])
              + " z=" + std::to_string(vals[2]));
}

// Test: Blob property default value from XML (hex encoded)
static void testBlobDefaultValue() {
    TEST("Blob property default value from XML (hex)");

    const char* xml = R"(
<EntityDef name="Item">
    <Properties>
        <Property name="signature" type="Blob" defaultValue="DEADBEEF"/>
    </Properties>
</EntityDef>
)";

    auto def = EntityDefLoader::loadFromString(xml);
    const auto* blobProp = def->findProperty("signature");

    bool ok = blobProp != nullptr;
    ok = ok && blobProp->defaultValue.size() == 4;
    if (blobProp->defaultValue.size() >= 4) {
        ok = ok && blobProp->defaultValue[0] == std::byte{0xDE};
        ok = ok && blobProp->defaultValue[1] == std::byte{0xAD};
        ok = ok && blobProp->defaultValue[2] == std::byte{0xBE};
        ok = ok && blobProp->defaultValue[3] == std::byte{0xEF};
    }

    if (ok) PASS();
    else FAIL("size=" + std::to_string(blobProp->defaultValue.size()));
}

// Test: string default value applied to entity at init
static void testStringDefaultAppliedToEntity() {
    TEST("String default value applied to entity at init");

    const char* xml = R"(
<EntityDef name="NPC">
    <Properties>
        <Property name="displayName" type="String" defaultValue="Unknown"/>
    </Properties>
</EntityDef>
)";

    auto def = EntityDefLoader::loadFromString(xml);
    theseed::runtime::Entity entity(1, theseed::runtime::EntitySide::Base, *def);

    auto val = entity.getString(0);
    bool ok = val == "Unknown";

    if (ok) PASS();
    else FAIL("expected 'Unknown', got '" + std::string(val) + "'");
}

// Test: Vector3 default value applied to entity at init
static void testVector3DefaultAppliedToEntity() {
    TEST("Vector3 default value applied to entity at init");

    const char* xml = R"(
<EntityDef name="SpawnPoint">
    <Properties>
        <Property name="position" type="Vector3" defaultValue="100,200,300"/>
    </Properties>
</EntityDef>
)";

    auto def = EntityDefLoader::loadFromString(xml);
    theseed::runtime::Entity entity(1, theseed::runtime::EntitySide::Base, *def);

    auto& pos = entity.getProperty<theseed::runtime::Vector3>(0);
    bool ok = std::abs(pos.x - 100.0f) < 0.01f;
    ok = ok && std::abs(pos.y - 200.0f) < 0.01f;
    ok = ok && std::abs(pos.z - 300.0f) < 0.01f;

    if (ok) PASS();
    else FAIL("x=" + std::to_string(pos.x) + " y=" + std::to_string(pos.y)
              + " z=" + std::to_string(pos.z));
}

int main() {
    std::cout << "EntityDefLoader tests:\n";

    testLoadBasicEntity();
    testLoadWithMethods();
    testLoadAllTypes();
    testLoadFixedSize();
    testLoadEmptyEntity();
    testLoadInvalidType();
    testLoadMethodWithArgs();
    testInheritance();
    testMultiLevelInheritance();
    testDuplicatePropertyRejected();
    testEmptyPropertyRejected();
    testStringDefaultValue();
    testVector3DefaultValue();
    testBlobDefaultValue();
    testStringDefaultAppliedToEntity();
    testVector3DefaultAppliedToEntity();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
