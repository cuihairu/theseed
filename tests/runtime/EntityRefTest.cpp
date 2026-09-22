#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/EntityRef.h"

#include <iostream>
#include <memory>
#include <string>
#include <utility>

using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntityId;
using theseed::runtime::EntityRef;
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

static EntityDef makeDef(const std::string& name) {
    EntityDef def(name);
    def.addProperty("hp", theseed::runtime::PropertyType::Int32, 0,
                    theseed::runtime::PropertyFlag::None, {});
    return def;
}

// Test 1: Create EntityRef from active entity
static void testCreateFromEntity() {
    TEST("EntityRef: create from active entity");

    auto def = makeDef("Avatar");
    auto entity = std::make_unique<Entity>(1, EntitySide::Base, def);

    EntityRef ref = entity->ref();
    bool ok = ref.isValid();
    ok = ok && ref.entityId() == 1;
    ok = ok && ref.get() == entity.get();
    ok = ok && &(*ref) == entity.get();
    ok = ok && ref->id() == 1;

    if (ok) PASS();
    else FAIL("ref not valid");
}

// Test 2: EntityRef invalidates after entity destruction
static void testInvalidationAfterDestroy() {
    TEST("EntityRef: invalidates after entity destroy");

    auto def = makeDef("Avatar");
    auto entity = std::make_unique<Entity>(42, EntitySide::Base, def);
    entity->activate();

    EntityRef ref = entity->ref();
    bool ok = ref.isValid();
    ok = ok && ref->id() == 42;

    entity->beginDestroy();
    entity->destroy();

    ok = ok && !ref.isValid();
    ok = ok && ref.get() == nullptr;
    ok = ok && ref.entityId() == 42;  // id preserved

    if (ok) PASS();
    else FAIL("ref should be invalid after destroy");
}

// Test 3: Default EntityRef is invalid
static void testDefaultInvalid() {
    TEST("EntityRef: default constructed is invalid");

    EntityRef ref;
    bool ok = !ref.isValid();
    ok = ok && !static_cast<bool>(ref);
    ok = ok && ref.get() == nullptr;
    ok = ok && ref.entityId() == 0;

    auto invalid = EntityRef::invalid();
    ok = ok && !invalid.isValid();

    if (ok) PASS();
    else FAIL("default ref should be invalid");
}

// Test 4: Copy EntityRef
static void testCopy() {
    TEST("EntityRef: copy preserves validity");

    auto def = makeDef("Avatar");
    auto entity = std::make_unique<Entity>(1, EntitySide::Cell, def);
    entity->activate();

    EntityRef original = entity->ref();
    EntityRef copy = original;

    bool ok = copy.isValid();
    ok = ok && copy.entityId() == 1;
    ok = ok && copy.get() == entity.get();

    entity->beginDestroy();
    entity->destroy();

    ok = ok && !copy.isValid();
    ok = ok && !original.isValid();

    if (ok) PASS();
    else FAIL("copy failed");
}

// Test 5: Move EntityRef
static void testMove() {
    TEST("EntityRef: move transfers ownership");

    auto def = makeDef("Avatar");
    auto entity = std::make_unique<Entity>(1, EntitySide::Base, def);
    entity->activate();

    EntityRef original = entity->ref();
    EntityRef moved = std::move(original);

    bool ok = moved.isValid();
    ok = ok && moved.entityId() == 1;
    ok = ok && moved.get() == entity.get();

    if (ok) PASS();
    else FAIL("move failed");
}

// Test 6: Assignment operator
static void testAssignment() {
    TEST("EntityRef: assignment operator");

    auto def1 = makeDef("Avatar");
    auto def2 = makeDef("NPC");
    auto e1 = std::make_unique<Entity>(1, EntitySide::Base, def1);
    auto e2 = std::make_unique<Entity>(2, EntitySide::Base, def2);
    e1->activate();
    e2->activate();

    EntityRef ref1 = e1->ref();
    EntityRef ref2 = e2->ref();

    ref1 = ref2;
    bool ok = ref1.isValid();
    ok = ok && ref1.entityId() == 2;
    ok = ok && ref1.get() == e2.get();

    // Destroy e2, both refs should invalidate
    e2->beginDestroy();
    e2->destroy();
    ok = ok && !ref1.isValid();
    ok = ok && !ref2.isValid();

    if (ok) PASS();
    else FAIL("assignment failed");
}

// Test 7: Reset invalidates ref
static void testReset() {
    TEST("EntityRef: reset invalidates ref");

    auto def = makeDef("Avatar");
    auto entity = std::make_unique<Entity>(1, EntitySide::Base, def);
    entity->activate();

    EntityRef ref = entity->ref();
    bool ok = ref.isValid();

    ref.reset();
    ok = ok && !ref.isValid();
    ok = ok && ref.get() == nullptr;
    ok = ok && ref.entityId() == 0;

    // Entity still alive
    ok = ok && entity->isActive();

    if (ok) PASS();
    else FAIL("reset failed");
}

// Test 8: Multiple refs to same entity
static void testMultipleRefs() {
    TEST("EntityRef: multiple refs invalidate together");

    auto def = makeDef("Avatar");
    auto entity = std::make_unique<Entity>(1, EntitySide::Cell, def);
    entity->activate();

    auto ref1 = entity->ref();
    auto ref2 = entity->ref();
    EntityRef ref3 = ref1;  // copy

    bool ok = ref1.isValid() && ref2.isValid() && ref3.isValid();

    entity->beginDestroy();
    entity->destroy();

    ok = ok && !ref1.isValid();
    ok = ok && !ref2.isValid();
    ok = ok && !ref3.isValid();

    if (ok) PASS();
    else FAIL("multiple refs should all invalidate");
}

// Test 9: EntityRef survives entity unique_ptr move
static void testRefAfterPtrMove() {
    TEST("EntityRef: valid after unique_ptr move");

    auto def = makeDef("Avatar");
    auto entity = std::make_unique<Entity>(1, EntitySide::Base, def);
    entity->activate();

    EntityRef ref = entity->ref();
    auto* rawPtr = entity.get();

    // Move entity to a new unique_ptr
    auto entity2 = std::move(entity);
    bool ok = ref.isValid();
    ok = ok && ref.get() == rawPtr;
    ok = ok && ref.get() == entity2.get();

    if (ok) PASS();
    else FAIL("ref should survive ptr move");
}

// Test 10: Ref from one entity reassigned to another
static void testReassignToDifferentEntity() {
    TEST("EntityRef: reassign to different entity");

    auto def1 = makeDef("Avatar");
    auto def2 = makeDef("NPC");
    auto e1 = std::make_unique<Entity>(1, EntitySide::Base, def1);
    auto e2 = std::make_unique<Entity>(2, EntitySide::Cell, def2);
    e1->activate();
    e2->activate();

    EntityRef ref = e1->ref();
    bool ok = ref.entityId() == 1;

    ref = e2->ref();
    ok = ok && ref.isValid();
    ok = ok && ref.entityId() == 2;
    ok = ok && ref.get() == e2.get();

    // Destroy e1 - ref should still be valid (points to e2)
    e1->beginDestroy();
    e1->destroy();
    ok = ok && ref.isValid();
    ok = ok && ref.entityId() == 2;

    if (ok) PASS();
    else FAIL("reassign failed");
}

// Test 11: EntityRef fromEntity static factory
static void testFromEntityStatic() {
    TEST("EntityRef: fromEntity static factory");

    auto def = makeDef("Avatar");
    auto entity = std::make_unique<Entity>(99, EntitySide::Base, def);
    entity->activate();

    auto ref = EntityRef::fromEntity(*entity);
    bool ok = ref.isValid();
    ok = ok && ref.entityId() == 99;

    if (ok) PASS();
    else FAIL("fromEntity failed");
}

int main() {
    std::cout << "EntityRef tests:\n";

    testCreateFromEntity();
    testInvalidationAfterDestroy();
    testDefaultInvalid();
    testCopy();
    testMove();
    testAssignment();
    testReset();
    testMultipleRefs();
    testRefAfterPtrMove();
    testReassignToDifferentEntity();
    testFromEntityStatic();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
