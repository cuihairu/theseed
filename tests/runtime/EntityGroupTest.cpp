#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/EntityRef.h"
#include "theseed/runtime/GroupManager.h"

#include <cstddef>
#include <iostream>
#include <memory>
#include <span>
#include <string>

using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntityGroup;
using theseed::runtime::EntityId;
using theseed::runtime::EntityRef;
using theseed::runtime::EntitySide;
using theseed::runtime::GroupManager;
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

static EntityDef makeDef(const std::string& name) {
    EntityDef def(name);
    def.addProperty("hp", PropertyType::Int32);
    def.addMethod("heal", theseed::runtime::MethodSide::Base);
    return def;
}

// Test 1: Create group, add members
static void testCreateAndAddMembers() {
    TEST("Group: create and add members");

    auto def = makeDef("Avatar");
    Entity e1(1, EntitySide::Base, def);
    Entity e2(2, EntitySide::Base, def);
    Entity e3(3, EntitySide::Base, def);

    GroupManager mgr;
    auto* group = mgr.createGroup(1);
    bool ok = group != nullptr;
    ok = ok && group->leader() == 1;
    ok = ok && group->memberCount() == 0;

    ok = ok && group->addMember(e1);
    ok = ok && group->addMember(e2);
    ok = ok && group->addMember(e3);
    ok = ok && group->memberCount() == 3;
    ok = ok && group->isMember(1) && group->isMember(2) && group->isMember(3);
    ok = ok && !group->isMember(99);

    // Duplicate add
    ok = ok && !group->addMember(e1);

    if (ok) PASS();
    else FAIL("create/add failed, count=" + std::to_string(group->memberCount()));
}

// Test 2: Remove members
static void testRemoveMembers() {
    TEST("Group: remove members");

    auto def = makeDef("Avatar");
    Entity e1(1, EntitySide::Base, def);
    Entity e2(2, EntitySide::Base, def);

    GroupManager mgr;
    auto* group = mgr.createGroup(1);
    group->addMember(e1);
    group->addMember(e2);

    bool ok = group->removeMember(2);
    ok = ok && group->memberCount() == 1;
    ok = ok && !group->isMember(2);

    // Remove non-member
    ok = ok && !group->removeMember(99);

    if (ok) PASS();
    else FAIL("remove failed");
}

// Test 3: Auto-invalidate when entity destroyed
static void testAutoInvalidateOnDestroy() {
    TEST("Group: auto-invalidate when entity destroyed");

    auto def = makeDef("Avatar");
    auto e1 = std::make_unique<Entity>(1, EntitySide::Base, def);
    auto e2 = std::make_unique<Entity>(2, EntitySide::Base, def);
    auto e3 = std::make_unique<Entity>(3, EntitySide::Base, def);
    e1->activate(); e2->activate(); e3->activate();

    GroupManager mgr;
    auto* group = mgr.createGroup(1);
    group->addMember(*e1);
    group->addMember(*e2);
    group->addMember(*e3);

    bool ok = group->activeMemberCount() == 3;

    // Destroy e2
    e2->beginDestroy();
    e2->destroy();

    ok = ok && group->memberCount() == 3;   // still 3 entries
    ok = ok && group->activeMemberCount() == 2;  // only 2 valid

    // Cleanup removes invalid entries
    group->cleanup();
    ok = ok && group->memberCount() == 2;
    ok = ok && !group->isMember(2);

    if (ok) PASS();
    else FAIL("invalidate failed, active=" + std::to_string(group->activeMemberCount()));
}

// Test 4: forEachMember iterates only active
static void testForEachMember() {
    TEST("Group: forEachMember skips destroyed entities");

    auto def = makeDef("Avatar");
    auto e1 = std::make_unique<Entity>(1, EntitySide::Base, def);
    auto e2 = std::make_unique<Entity>(2, EntitySide::Base, def);
    e1->activate(); e2->activate();

    GroupManager mgr;
    auto* group = mgr.createGroup(1);
    group->addMember(*e1);
    group->addMember(*e2);

    e2->beginDestroy();
    e2->destroy();

    std::vector<EntityId> visited;
    group->forEachMember([&](Entity& e) {
        visited.push_back(e.id());
    });

    bool ok = visited.size() == 1;
    ok = ok && visited[0] == 1;

    if (ok) PASS();
    else FAIL("forEach visited " + std::to_string(visited.size()) + " entities");
}

// Test 5: activeMembers returns valid pointers
static void testActiveMembers() {
    TEST("Group: activeMembers returns valid entities");

    auto def = makeDef("Avatar");
    Entity e1(1, EntitySide::Base, def);
    Entity e2(2, EntitySide::Base, def);
    Entity e3(3, EntitySide::Base, def);

    GroupManager mgr;
    auto* group = mgr.createGroup(1);
    group->addMember(e1);
    group->addMember(e2);
    group->addMember(e3);

    auto members = group->activeMembers();
    bool ok = members.size() == 3;

    if (ok) PASS();
    else FAIL("activeMembers count=" + std::to_string(members.size()));
}

// Test 6: Broadcast to all members
static void testBroadcast() {
    TEST("Group: broadcast method to all members");

    auto def = makeDef("Avatar");
    Entity e1(1, EntitySide::Base, def);
    Entity e2(2, EntitySide::Base, def);
    Entity e3(3, EntitySide::Base, def);

    int callCount = 0;
    std::vector<EntityId> called;

    auto setupHandler = [&](Entity& e) {
        e.bindMethodHandler("heal", [&](Entity& entity, std::span<const std::byte>) {
            ++callCount;
            called.push_back(entity.id());
        });
    };
    setupHandler(e1);
    setupHandler(e2);
    setupHandler(e3);

    GroupManager mgr;
    auto* group = mgr.createGroup(1);
    group->addMember(e1);
    group->addMember(e2);
    group->addMember(e3);

    group->broadcast("heal");

    bool ok = callCount == 3;

    if (ok) PASS();
    else FAIL("broadcast count=" + std::to_string(callCount));
}

// Test 7: Destroy group
static void testDestroyGroup() {
    TEST("GroupManager: destroy group");

    GroupManager mgr;
    auto* g1 = mgr.createGroup(1);
    auto* g2 = mgr.createGroup(2);

    // destroy 后 g1 悬垂，id 需提前留存。
    const auto id1 = g1->id();

    bool ok = mgr.groupCount() == 2;
    ok = ok && mgr.destroyGroup(id1);
    ok = ok && mgr.groupCount() == 1;
    ok = ok && mgr.findGroup(id1) == nullptr;
    ok = ok && mgr.findGroup(g2->id()) == g2;

    // Double destroy
    ok = ok && !mgr.destroyGroup(id1);

    if (ok) PASS();
    else FAIL("destroy failed");
}

// Test 8: Member added/removed callbacks
static void testMemberCallbacks() {
    TEST("Group: member added/removed callbacks");

    auto def = makeDef("Avatar");
    Entity e1(1, EntitySide::Base, def);
    Entity e2(2, EntitySide::Base, def);

    GroupManager mgr;
    auto* group = mgr.createGroup(1);

    EntityId addedId = 0, removedId = 0;
    group->setOnMemberAdded([&](EntityGroup&, EntityId id) { addedId = id; });
    group->setOnMemberRemoved([&](EntityGroup&, EntityId id) { removedId = id; });

    group->addMember(e1);
    bool ok = addedId == 1;

    group->addMember(e2);
    ok = ok && addedId == 2;

    group->removeMember(1);
    ok = ok && removedId == 1;

    if (ok) PASS();
    else FAIL("callbacks failed");
}

// Test 9: findGroupByMember / groupsForMember
static void testFindByMember() {
    TEST("GroupManager: findGroupByMember and groupsForMember");

    auto def = makeDef("Avatar");
    Entity e1(1, EntitySide::Base, def);
    Entity e2(2, EntitySide::Base, def);
    Entity e3(3, EntitySide::Base, def);

    GroupManager mgr;
    auto* g1 = mgr.createGroup(1);
    auto* g2 = mgr.createGroup(2);

    g1->addMember(e1);
    g1->addMember(e2);
    g2->addMember(e2);
    g2->addMember(e3);

    // e2 is in both groups
    auto groups = mgr.groupsForMember(2);
    bool ok = groups.size() == 2;

    // e1 is only in g1
    ok = ok && mgr.findGroupByMember(1) == g1;

    // e3 is only in g2
    ok = ok && mgr.findGroupByMember(3) == g2;

    // e99 is not in any group
    ok = ok && mgr.findGroupByMember(99) == nullptr;

    if (ok) PASS();
    else FAIL("findByMember failed, groups=" + std::to_string(groups.size()));
}

// Test 10: Leader management
static void testLeaderManagement() {
    TEST("Group: leader management");

    GroupManager mgr;
    auto* group = mgr.createGroup(1);

    bool ok = group->leader() == 1;

    group->setLeader(2);
    ok = ok && group->leader() == 2;

    if (ok) PASS();
    else FAIL("leader management failed");
}

// Test 11: GroupManager cleanupAll
static void testCleanupAll() {
    TEST("GroupManager: cleanupAll removes invalid members from all groups");

    auto def = makeDef("Avatar");
    auto e1 = std::make_unique<Entity>(1, EntitySide::Base, def);
    auto e2 = std::make_unique<Entity>(2, EntitySide::Base, def);
    auto e3 = std::make_unique<Entity>(3, EntitySide::Base, def);
    e1->activate(); e2->activate(); e3->activate();

    GroupManager mgr;
    auto* g1 = mgr.createGroup(1);
    auto* g2 = mgr.createGroup(2);

    g1->addMember(*e1);
    g1->addMember(*e2);
    g2->addMember(*e2);
    g2->addMember(*e3);

    // Destroy e2
    e2->beginDestroy();
    e2->destroy();

    mgr.cleanupAll();

    bool ok = g1->memberCount() == 1;
    ok = ok && g1->isMember(1) && !g1->isMember(2);
    ok = ok && g2->memberCount() == 1;
    ok = ok && g2->isMember(3) && !g2->isMember(2);

    if (ok) PASS();
    else FAIL("cleanupAll failed");
}

// 死成员跳过臂：activeMembers / broadcast 的 ref.get() 假臂与
// groupsForMember 的 isMember 假臂（存活成员遍历已在前面场景覆盖）。
static void testDeadMemberSkipsInQueries() {
    TEST("Group: dead members skipped in activeMembers/broadcast/groupsForMember");

    auto def = makeDef("Avatar");
    auto alive = std::make_unique<Entity>(1, EntitySide::Base, def);
    auto dead = std::make_unique<Entity>(2, EntitySide::Base, def);
    alive->activate();
    dead->activate();

    GroupManager mgr;
    auto* group = mgr.createGroup(1);
    group->addMember(*alive);
    group->addMember(*dead);

    int handlerCalls = 0;
    auto setupHandler = [&](Entity& e) {
        e.bindMethodHandler("heal", [&](Entity&, std::span<const std::byte>) {
            handlerCalls += 1;
        });
    };
    setupHandler(*alive);
    setupHandler(*dead);

    dead->beginDestroy();
    dead->destroy();

    // activeMembers：死成员 get() 为 null → 跳过
    auto members = group->activeMembers();
    bool ok = members.size() == 1 && members[0]->id() == 1;

    // broadcast：死成员跳过、不投递（若未跳过则 handlerCalls == 2）
    group->broadcast("heal");
    ok = ok && handlerCalls == 1;

    // groupsForMember：未入组 id 在每组 isMember 假臂上被跳过
    ok = ok && mgr.groupsForMember(99).empty();
    ok = ok && mgr.groupsForMember(1).size() == 1;

    if (ok) PASS();
    else FAIL("dead member skip failed, calls=" + std::to_string(handlerCalls));
}

int main() {
    std::cout << "Entity group tests:\n";

    testCreateAndAddMembers();
    testRemoveMembers();
    testAutoInvalidateOnDestroy();
    testForEachMember();
    testActiveMembers();
    testBroadcast();
    testDestroyGroup();
    testMemberCallbacks();
    testFindByMember();
    testLeaderManagement();
    testCleanupAll();
    testDeadMemberSkipsInQueries();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
