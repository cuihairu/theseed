#include "theseed/core/EntityDefLoader.h"
#include "theseed/runtime/EntityDef.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

using theseed::core::EntityDefLoader;
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

int main() {
    std::cout << "EntityDefLoader tests:\n";

    testLoadBasicEntity();
    testLoadWithMethods();
    testLoadAllTypes();
    testLoadFixedSize();
    testLoadEmptyEntity();
    testLoadInvalidType();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
