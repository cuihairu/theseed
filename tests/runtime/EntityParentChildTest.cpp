#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"

#include <iostream>
#include <memory>
#include <string>

using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntityId;
using theseed::runtime::EntitySide;

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

static void testSetGetParent() {
    TEST("setParent/parent/hasParent basics");

    EntityDef def("Item");
    Entity e(1, EntitySide::Base, def);

    bool ok = !e.hasParent() && e.parent() == 0;

    e.setParent(42);
    ok = ok && e.hasParent() && e.parent() == 42;

    if (ok) PASS();
    else FAIL("parent mismatch, parent=" + std::to_string(e.parent()));
}

static void testAddRemoveChild() {
    TEST("addChild/removeChild/children basics");

    EntityDef def("Player");
    Entity parent(1, EntitySide::Base, def);

    parent.addChild(10);
    parent.addChild(20);

    bool ok = parent.hasChildren() && parent.childCount() == 2;
    ok = ok && parent.children().contains(10);
    ok = ok && parent.children().contains(20);

    parent.removeChild(10);
    ok = ok && parent.childCount() == 1;
    ok = ok && !parent.children().contains(10);

    if (ok) PASS();
    else FAIL("count=" + std::to_string(parent.childCount()));
}

static void testNoSelfAsChild() {
    TEST("addChild rejects self as child");

    EntityDef def("Entity");
    Entity e(5, EntitySide::Base, def);

    e.addChild(5);
    e.addChild(0);

    bool ok = e.childCount() == 0 && !e.hasChildren();

    if (ok) PASS();
    else FAIL("count=" + std::to_string(e.childCount()));
}

static void testDuplicateChild() {
    TEST("duplicate addChild is idempotent");

    EntityDef def("Entity");
    Entity e(1, EntitySide::Base, def);

    e.addChild(10);
    e.addChild(10);

    if (e.childCount() == 1) PASS();
    else FAIL("count=" + std::to_string(e.childCount()));
}

static void testRemoveNonexistentChild() {
    TEST("removeChild for non-existent child is safe");

    EntityDef def("Entity");
    Entity e(1, EntitySide::Base, def);

    e.removeChild(999);
    e.addChild(10);
    e.removeChild(999);

    if (e.childCount() == 1) PASS();
    else FAIL("count=" + std::to_string(e.childCount()));
}

static void testMultipleChildren() {
    TEST("entity can have many children");

    EntityDef def("Inventory");
    Entity e(1, EntitySide::Base, def);

    for (EntityId i = 100; i < 110; ++i) {
        e.addChild(i);
    }

    bool ok = e.childCount() == 10;
    ok = ok && e.children().contains(105);

    if (ok) PASS();
    else FAIL("count=" + std::to_string(e.childCount()));
}

static void testDestroyClearsRelationships() {
    TEST("destroy clears parent ref and children");

    EntityDef def("Entity");
    Entity e(1, EntitySide::Base, def);
    e.activate();

    e.setParent(50);
    e.addChild(10);
    e.addChild(20);

    bool ok = e.hasParent() && e.childCount() == 2;

    e.beginDestroy();
    e.destroy();
    ok = ok && !e.hasParent() && e.parent() == 0;
    ok = ok && !e.hasChildren() && e.childCount() == 0;

    if (ok) PASS();
    else FAIL("parent=" + std::to_string(e.parent())
              + " children=" + std::to_string(e.childCount()));
}

static void testParentNotClearedBySetParentZero() {
    TEST("setParent(0) clears parent reference");

    EntityDef def("Entity");
    Entity e(1, EntitySide::Base, def);

    e.setParent(42);
    bool ok = e.hasParent();

    e.setParent(0);
    ok = ok && !e.hasParent() && e.parent() == 0;

    if (ok) PASS();
    else FAIL("parent=" + std::to_string(e.parent()));
}

static void testDefaultState() {
    TEST("entity has no parent/children by default");

    EntityDef def("Entity");
    Entity e(1, EntitySide::Base, def);

    bool ok = !e.hasParent() && e.parent() == 0;
    ok = ok && !e.hasChildren() && e.childCount() == 0;
    ok = ok && e.children().empty();

    if (ok) PASS();
    else FAIL("unexpected default state");
}

int main() {
    std::cout << "EntityParentChild tests:\n";

    testDefaultState();
    testSetGetParent();
    testAddRemoveChild();
    testNoSelfAsChild();
    testDuplicateChild();
    testRemoveNonexistentChild();
    testMultipleChildren();
    testDestroyClearsRelationships();
    testParentNotClearedBySetParentZero();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
