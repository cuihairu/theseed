#include "theseed/core/EntityDefLoader.h"
#include "theseed/core/BaseRuntime.h"
#include "theseed/core/IEntityStore.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/RuntimeTransport.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

using theseed::core::BaseRuntime;
using theseed::core::EntityDefLoader;
using theseed::core::InMemoryEntityStore;
using theseed::runtime::DeliveryClass;
using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntityId;
using theseed::runtime::EntitySide;
using theseed::runtime::hasFlag;
using theseed::runtime::InMemoryRuntimeTransport;
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

static void testPropertyFlags() {
    TEST("addProperty with Persistent and ClientSync flags");

    EntityDef def("Avatar");
    def.addProperty("hp", PropertyType::Int32, 4, PropertyFlag::Persistent);
    def.addProperty("name", PropertyType::String, 0, PropertyFlag::ClientSync);
    def.addProperty("level", PropertyType::Int32, 4,
                    PropertyFlag::Persistent | PropertyFlag::ClientSync);
    def.addProperty("internal", PropertyType::Int32, 4, PropertyFlag::None);

    const auto& props = def.properties();
    bool ok = hasFlag(props[0].flags, PropertyFlag::Persistent)
           && !hasFlag(props[0].flags, PropertyFlag::ClientSync)
           && hasFlag(props[1].flags, PropertyFlag::ClientSync)
           && !hasFlag(props[1].flags, PropertyFlag::Persistent)
           && hasFlag(props[2].flags, PropertyFlag::Persistent)
           && hasFlag(props[2].flags, PropertyFlag::ClientSync)
           && props[3].flags == PropertyFlag::None;

    if (ok) PASS();
    else FAIL("flag mismatch");
}

static void testDefaultValueInt32() {
    TEST("default value applied to Int32 property on init");

    EntityDef def("Avatar");
    std::vector<std::byte> defaultHp(sizeof(int32_t));
    int32_t hpValue = 100;
    std::memcpy(defaultHp.data(), &hpValue, sizeof(hpValue));
    def.addProperty("hp", PropertyType::Int32, 4, PropertyFlag::Persistent, std::move(defaultHp));

    Entity entity(1, EntitySide::Base, def);
    auto hp = entity.getProperty<int32_t>(0);

    if (hp == 100) PASS();
    else FAIL("expected hp=100, got " + std::to_string(hp));
}

static void testDefaultValueFloat32() {
    TEST("default value applied to Float32 property on init");

    EntityDef def("Item");
    std::vector<std::byte> defaultWeight(sizeof(float));
    float w = 3.14f;
    std::memcpy(defaultWeight.data(), &w, sizeof(w));
    def.addProperty("weight", PropertyType::Float32, 4, PropertyFlag::None, std::move(defaultWeight));

    Entity entity(1, EntitySide::Cell, def);
    auto weight = entity.getProperty<float>(0);

    if (weight > 3.13f && weight < 3.15f) PASS();
    else FAIL("weight mismatch");
}

static void testNoDefaultValueIsZero() {
    TEST("property without default value is zero-initialized");

    EntityDef def("Avatar");
    def.addProperty("score", PropertyType::Int32, 4);

    Entity entity(1, EntitySide::Base, def);
    auto score = entity.getProperty<int32_t>(0);

    if (score == 0) PASS();
    else FAIL("expected 0, got " + std::to_string(score));
}

static void testPersistentFilterInSave() {
    TEST("auto-save only includes Persistent properties");

    auto transport = std::make_shared<InMemoryRuntimeTransport>();
    auto store = std::make_shared<InMemoryEntityStore>();
    BaseRuntime rt(transport, store, 1);

    auto def = std::make_shared<EntityDef>("Avatar");
    def->addProperty("hp", PropertyType::Int32, 4, PropertyFlag::Persistent);
    def->addProperty("temp", PropertyType::Int32, 4, PropertyFlag::ClientSync);

    rt.registerEntityFactory("Avatar",
        [def](EntityId id, EntitySide side) -> std::unique_ptr<Entity> {
            return std::make_unique<Entity>(id, side, *def);
        });

    auto* entity = rt.createEntity("Avatar");
    entity->setProperty<int32_t>(0, 200);
    entity->setProperty<int32_t>(1, 999);

    rt.saveEntity(entity->id());

    theseed::core::EntityData data;
    store->load(entity->id(), "Avatar", data);

    bool ok = data.properties.size() == 1
           && data.properties[0].name == "hp";

    if (ok) PASS();
    else FAIL("expected 1 property (hp), got " + std::to_string(data.properties.size()));
}

static void testXmlLoadWithFlags() {
    TEST("EntityDefLoader parses flags and defaultValue from XML");

    const std::string xml = R"(
        <EntityDef name="Avatar">
            <Properties>
                <Property name="hp" type="Int32" flags="Persistent" defaultValue="100"/>
                <Property name="name" type="String" flags="ClientSync"/>
                <Property name="level" type="Int32" flags="Persistent,ClientSync" defaultValue="1"/>
                <Property name="internal" type="Int32"/>
            </Properties>
        </EntityDef>
    )";

    auto def = EntityDefLoader::loadFromString(xml);
    const auto& props = def->properties();

    bool ok = props.size() == 4
           && hasFlag(props[0].flags, PropertyFlag::Persistent)
           && props[0].defaultValue.size() == 4
           && hasFlag(props[1].flags, PropertyFlag::ClientSync)
           && hasFlag(props[2].flags, PropertyFlag::Persistent)
           && hasFlag(props[2].flags, PropertyFlag::ClientSync)
           && props[3].flags == PropertyFlag::None;

    if (!ok) {
        FAIL("parsed flags/default mismatch");
        return;
    }

    int32_t hpDefault = 0;
    std::memcpy(&hpDefault, props[0].defaultValue.data(), 4);
    int32_t levelDefault = 0;
    std::memcpy(&levelDefault, props[2].defaultValue.data(), 4);

    if (hpDefault == 100 && levelDefault == 1) PASS();
    else FAIL("defaultValue mismatch, hp=" + std::to_string(hpDefault) + " level=" + std::to_string(levelDefault));
}

static void testXmlDefaultAppliedToEntity() {
    TEST("XML-defined default value applied to entity property");

    const std::string xml = R"(
        <EntityDef name="Player">
            <Properties>
                <Property name="health" type="Int32" flags="Persistent" defaultValue="250"/>
            </Properties>
        </EntityDef>
    )";

    auto def = EntityDefLoader::loadFromString(xml);
    Entity entity(42, EntitySide::Base, *def);

    auto health = entity.getProperty<int32_t>(0);
    if (health == 250) PASS();
    else FAIL("expected 250, got " + std::to_string(health));
}

int main() {
    std::cout << "PropertyMetadata tests:\n";

    testPropertyFlags();
    testDefaultValueInt32();
    testDefaultValueFloat32();
    testNoDefaultValueIsZero();
    testPersistentFilterInSave();
    testXmlLoadWithFlags();
    testXmlDefaultAppliedToEntity();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
