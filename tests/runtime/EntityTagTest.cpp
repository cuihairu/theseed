#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/Space.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntityId;
using theseed::runtime::EntitySide;
using theseed::runtime::SingleCellTopology;
using theseed::runtime::Space;
using theseed::runtime::Vector3;

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

static std::unique_ptr<Space> makeSpace() {
    auto space = std::make_unique<Space>(1, "test", std::make_unique<SingleCellTopology>(1));
    space->initialize();
    return space;
}

static void testAddAndHasTag() {
    TEST("add and check tags on entity");

    EntityDef def("NPC");
    Entity e(1, EntitySide::Cell, def);

    e.addTag("enemy");
    e.addTag("loot");

    bool ok = e.hasTag("enemy") && e.hasTag("loot") && !e.hasTag("friend");
    if (ok) PASS();
    else FAIL("tag mismatch");
}

static void testRemoveTag() {
    TEST("remove tag from entity");

    EntityDef def("NPC");
    Entity e(1, EntitySide::Cell, def);

    e.addTag("enemy");
    e.removeTag("enemy");

    if (!e.hasTag("enemy") && e.tags().empty()) PASS();
    else FAIL("tag not removed");
}

static void testDuplicateTag() {
    TEST("duplicate addTag is idempotent");

    EntityDef def("NPC");
    Entity e(1, EntitySide::Cell, def);

    e.addTag("boss");
    e.addTag("boss");

    if (e.tags().size() == 1) PASS();
    else FAIL("expected 1 tag, got " + std::to_string(e.tags().size()));
}

static void testTagsEmptyByDefault() {
    TEST("entity has no tags by default");

    EntityDef def("NPC");
    Entity e(1, EntitySide::Cell, def);

    if (e.tags().empty()) PASS();
    else FAIL("expected empty tags");
}

static void testFindByType() {
    TEST("Space::findEntitiesByType returns matching entities");

    auto space = makeSpace();

    EntityDef npcDef("NPC");
    EntityDef playerDef("Player");

    auto npc1 = std::make_unique<Entity>(1, EntitySide::Cell, npcDef);
    auto npc2 = std::make_unique<Entity>(2, EntitySide::Cell, npcDef);
    auto player = std::make_unique<Entity>(3, EntitySide::Cell, playerDef);

    space->addEntity(*npc1, Vector3{0, 0, 0});
    space->addEntity(*npc2, Vector3{10, 0, 0});
    space->addEntity(*player, Vector3{5, 0, 0});

    auto npcs = space->findEntitiesByType("NPC");
    auto players = space->findEntitiesByType("Player");
    auto monsters = space->findEntitiesByType("Monster");

    bool ok = npcs.size() == 2 && players.size() == 1 && monsters.empty();
    if (ok) PASS();
    else FAIL("counts mismatch");
}

static void testFindByTag() {
    TEST("Space::findEntitiesByTag returns tagged entities");

    auto space = makeSpace();

    EntityDef def("Entity");

    auto e1 = std::make_unique<Entity>(1, EntitySide::Cell, def);
    auto e2 = std::make_unique<Entity>(2, EntitySide::Cell, def);
    auto e3 = std::make_unique<Entity>(3, EntitySide::Cell, def);

    e1->addTag("enemy");
    e2->addTag("friendly");
    e3->addTag("enemy");

    space->addEntity(*e1, Vector3{0, 0, 0});
    space->addEntity(*e2, Vector3{0, 0, 0});
    space->addEntity(*e3, Vector3{0, 0, 0});

    auto enemies = space->findEntitiesByTag("enemy");
    auto friendlies = space->findEntitiesByTag("friendly");
    auto unknowns = space->findEntitiesByTag("boss");

    bool ok = enemies.size() == 2 && friendlies.size() == 1 && unknowns.empty();
    if (ok) PASS();
    else FAIL("counts mismatch");
}

static void testFindByTagAfterRemove() {
    TEST("findEntitiesByTag reflects tag removal");

    auto space = makeSpace();

    EntityDef def("Entity");
    auto e1 = std::make_unique<Entity>(1, EntitySide::Cell, def);
    e1->addTag("target");
    space->addEntity(*e1, Vector3{0, 0, 0});

    auto before = space->findEntitiesByTag("target").size();
    e1->removeTag("target");
    auto after = space->findEntitiesByTag("target").size();

    if (before == 1 && after == 0) PASS();
    else FAIL("expected before=1, after=0");
}

static void testFindByTagMultipleTags() {
    TEST("entity with multiple tags found by each");

    auto space = makeSpace();

    EntityDef def("Entity");
    auto e1 = std::make_unique<Entity>(1, EntitySide::Cell, def);
    e1->addTag("elite");
    e1->addTag("boss");
    e1->addTag("flying");
    space->addEntity(*e1, Vector3{0, 0, 0});

    auto elites = space->findEntitiesByTag("elite");
    auto bosses = space->findEntitiesByTag("boss");
    auto flyers = space->findEntitiesByTag("flying");
    auto ground = space->findEntitiesByTag("ground");

    bool ok = elites.size() == 1 && bosses.size() == 1
           && flyers.size() == 1 && ground.empty();
    if (ok) PASS();
    else FAIL("multi-tag query failed");
}

int main() {
    std::cout << "EntityTag tests:\n";

    testAddAndHasTag();
    testRemoveTag();
    testDuplicateTag();
    testTagsEmptyByDefault();
    testFindByType();
    testFindByTag();
    testFindByTagAfterRemove();
    testFindByTagMultipleTags();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
