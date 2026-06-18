#include "theseed/runtime/BehaviorTree.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"

#include <iostream>
#include <memory>

using theseed::runtime::BehaviorStatus;
using theseed::runtime::BehaviorTree;
using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntitySide;
using namespace theseed::runtime::bt;

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

static EntityDef makeDef() {
    EntityDef def("NPC");
    def.addProperty("hp", theseed::runtime::PropertyType::Int32);
    return def;
}

// Test 1: Sequence succeeds when all children succeed
static void testSequenceSuccess() {
    TEST("Sequence: all children succeed");

    auto def = makeDef();
    Entity e(1, EntitySide::Cell, def);

    int step1 = 0, step2 = 0, step3 = 0;
    auto seq = sequence();
    seq->addChild(action([&](Entity&) { ++step1; return BehaviorStatus::Success; }));
    seq->addChild(action([&](Entity&) { ++step2; return BehaviorStatus::Success; }));
    seq->addChild(action([&](Entity&) { ++step3; return BehaviorStatus::Success; }));

    BehaviorTree tree(std::move(seq));
    auto status = tree.tick(e);

    bool ok = status == BehaviorStatus::Success;
    ok = ok && step1 == 1 && step2 == 1 && step3 == 1;

    if (ok) PASS();
    else FAIL("steps=" + std::to_string(step1+step2+step3));
}

// Test 2: Sequence fails when a child fails
static void testSequenceFailure() {
    TEST("Sequence: fails when child fails");

    auto def = makeDef();
    Entity e(1, EntitySide::Cell, def);

    int step1 = 0, step2 = 0, step3 = 0;
    auto seq = sequence();
    seq->addChild(action([&](Entity&) { ++step1; return BehaviorStatus::Success; }));
    seq->addChild(action([&](Entity&) { ++step2; return BehaviorStatus::Failure; }));
    seq->addChild(action([&](Entity&) { ++step3; return BehaviorStatus::Success; }));

    BehaviorTree tree(std::move(seq));
    auto status = tree.tick(e);

    bool ok = status == BehaviorStatus::Failure;
    ok = ok && step1 == 1 && step2 == 1 && step3 == 0;

    if (ok) PASS();
    else FAIL("step3 should be 0");
}

// Test 3: Sequence with running child
static void testSequenceRunning() {
    TEST("Sequence: running child pauses sequence");

    auto def = makeDef();
    Entity e(1, EntitySide::Cell, def);

    int step1 = 0, step2Calls = 0;
    auto seq = sequence();
    seq->addChild(action([&](Entity&) { ++step1; return BehaviorStatus::Success; }));
    seq->addChild(action([&](Entity&) {
        ++step2Calls;
        return step2Calls >= 3 ? BehaviorStatus::Success : BehaviorStatus::Running;
    }));

    BehaviorTree tree(std::move(seq));

    auto s1 = tree.tick(e);
    bool ok = s1 == BehaviorStatus::Running && step1 == 1 && step2Calls == 1;

    auto s2 = tree.tick(e);
    ok = ok && s2 == BehaviorStatus::Running && step2Calls == 2;

    auto s3 = tree.tick(e);
    ok = ok && s3 == BehaviorStatus::Success && step2Calls == 3;

    if (ok) PASS();
    else FAIL("running failed, step2=" + std::to_string(step2Calls));
}

// Test 4: Selector succeeds on first success
static void testSelectorSuccess() {
    TEST("Selector: first success wins");

    auto def = makeDef();
    Entity e(1, EntitySide::Cell, def);

    int step1 = 0, step2 = 0, step3 = 0;
    auto sel = selector();
    sel->addChild(action([&](Entity&) { ++step1; return BehaviorStatus::Failure; }));
    sel->addChild(action([&](Entity&) { ++step2; return BehaviorStatus::Success; }));
    sel->addChild(action([&](Entity&) { ++step3; return BehaviorStatus::Success; }));

    BehaviorTree tree(std::move(sel));
    auto status = tree.tick(e);

    bool ok = status == BehaviorStatus::Success;
    ok = ok && step1 == 1 && step2 == 1 && step3 == 0;

    if (ok) PASS();
    else FAIL("selector failed");
}

// Test 5: Selector fails when all fail
static void testSelectorAllFail() {
    TEST("Selector: fails when all children fail");

    auto def = makeDef();
    Entity e(1, EntitySide::Cell, def);

    auto sel = selector();
    sel->addChild(action([&](Entity&) { return BehaviorStatus::Failure; }));
    sel->addChild(action([&](Entity&) { return BehaviorStatus::Failure; }));

    BehaviorTree tree(std::move(sel));
    bool ok = tree.tick(e) == BehaviorStatus::Failure;

    if (ok) PASS();
    else FAIL("should fail");
}

// Test 6: Condition guard
static void testConditionGuard() {
    TEST("Condition: blocks child when false");

    auto def = makeDef();
    Entity e(1, EntitySide::Cell, def);
    e.setProperty<std::int32_t>("hp", 0);

    int actionCalls = 0;
    auto cond = condition(
        [](Entity& e) { return *e.findProperty<std::int32_t>("hp") > 0; },
        action([&](Entity&) { ++actionCalls; return BehaviorStatus::Success; })
    );

    BehaviorTree tree(std::move(cond));

    // hp=0 → condition fails
    bool ok = tree.tick(e) == BehaviorStatus::Failure;
    ok = ok && actionCalls == 0;

    // Set hp > 0
    e.setProperty<std::int32_t>("hp", 50);
    ok = ok && tree.tick(e) == BehaviorStatus::Success;
    ok = ok && actionCalls == 1;

    if (ok) PASS();
    else FAIL("condition guard failed");
}

// Test 7: Inverter
static void testInverter() {
    TEST("Inverter: inverts success and failure");

    auto def = makeDef();
    Entity e(1, EntitySide::Cell, def);

    auto inv1 = inverter(action([](Entity&) { return BehaviorStatus::Success; }));
    BehaviorTree t1(std::move(inv1));
    bool ok = t1.tick(e) == BehaviorStatus::Failure;

    auto inv2 = inverter(action([](Entity&) { return BehaviorStatus::Failure; }));
    BehaviorTree t2(std::move(inv2));
    ok = ok && t2.tick(e) == BehaviorStatus::Success;

    if (ok) PASS();
    else FAIL("inverter failed");
}

// Test 8: Repeat node
static void testRepeatNode() {
    TEST("Repeat: repeats child N times");

    auto def = makeDef();
    Entity e(1, EntitySide::Cell, def);

    int count = 0;
    auto rep = repeat(
        action([&](Entity&) { ++count; return BehaviorStatus::Success; }),
        3
    );

    BehaviorTree tree(std::move(rep));

    // First tick starts iteration 1, returns Running (need more ticks)
    auto s1 = tree.tick(e);
    bool ok = s1 == BehaviorStatus::Running;

    auto s2 = tree.tick(e);
    ok = ok && s2 == BehaviorStatus::Running;

    auto s3 = tree.tick(e);
    ok = ok && s3 == BehaviorStatus::Success;
    ok = ok && count == 3;

    if (ok) PASS();
    else FAIL("repeat count=" + std::to_string(count));
}

// Test 9: Succeeder always succeeds
static void testSucceeder() {
    TEST("Succeeder: always returns success");

    auto def = makeDef();
    Entity e(1, EntitySide::Cell, def);

    auto suc = succeeder(action([](Entity&) { return BehaviorStatus::Failure; }));
    BehaviorTree tree(std::move(suc));

    bool ok = tree.tick(e) == BehaviorStatus::Success;

    if (ok) PASS();
    else FAIL("succeeder failed");
}

// Test 10: Nested composite (selector with sequences)
static void testNestedTree() {
    TEST("Nested: selector with sequence children");

    auto def = makeDef();
    Entity e(1, EntitySide::Cell, def);

    bool patrolDone = false, attackDone = false;
    bool hasTarget = false;

    auto patrol = sequence();
    patrol->addChild(action([&](Entity&) { return BehaviorStatus::Success; }));
    patrol->addChild(action([&](Entity&) { patrolDone = true; return BehaviorStatus::Success; }));

    auto attack = sequence();
    attack->addChild(action([&](Entity&) { return BehaviorStatus::Success; }));
    attack->addChild(action([&](Entity&) { attackDone = true; return BehaviorStatus::Success; }));

    auto root = selector();
    root->addChild(condition([&](Entity&) { return hasTarget; }, std::move(attack)));
    root->addChild(std::move(patrol));

    BehaviorTree tree(std::move(root));

    // No target → patrol
    tree.tick(e);
    bool ok = !attackDone && patrolDone;

    // Reset and set target → attack
    tree.reset();
    patrolDone = false;
    hasTarget = true;

    tree.tick(e);
    ok = ok && attackDone && !patrolDone;

    if (ok) PASS();
    else FAIL("nested tree failed");
}

// Test 11: Reset allows re-execution
static void testResetReExecution() {
    TEST("Reset: allows full re-execution");

    auto def = makeDef();
    Entity e(1, EntitySide::Cell, def);

    int count = 0;
    auto seq = sequence();
    seq->addChild(action([&](Entity&) { ++count; return BehaviorStatus::Success; }));
    seq->addChild(action([&](Entity&) { ++count; return BehaviorStatus::Success; }));

    BehaviorTree tree(std::move(seq));
    tree.tick(e);
    bool ok = count == 2;

    tree.reset();
    tree.tick(e);
    ok = ok && count == 4;

    if (ok) PASS();
    else FAIL("reset count=" + std::to_string(count));
}

// Test 12: isRunning flag
static void testIsRunningFlag() {
    TEST("BehaviorTree: isRunning flag");

    auto def = makeDef();
    Entity e(1, EntitySide::Cell, def);

    int calls = 0;
    auto seq = action([&](Entity&) {
        ++calls;
        return calls >= 3 ? BehaviorStatus::Success : BehaviorStatus::Running;
    });

    BehaviorTree tree(std::move(seq));

    tree.tick(e);
    bool ok = tree.isRunning();

    tree.tick(e);
    ok = ok && tree.isRunning();

    tree.tick(e);
    ok = ok && !tree.isRunning();

    if (ok) PASS();
    else FAIL("isRunning failed");
}

int main() {
    std::cout << "Behavior tree tests:\n";

    testSequenceSuccess();
    testSequenceFailure();
    testSequenceRunning();
    testSelectorSuccess();
    testSelectorAllFail();
    testConditionGuard();
    testInverter();
    testRepeatNode();
    testSucceeder();
    testNestedTree();
    testResetReExecution();
    testIsRunningFlag();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
