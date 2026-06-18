#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/StateMachine.h"

#include <iostream>
#include <memory>
#include <string>

using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntityId;
using theseed::runtime::EntitySide;
using theseed::runtime::StateMachine;

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
    EntityDef def("Avatar");
    def.addProperty("hp", theseed::runtime::PropertyType::Int32);
    return def;
}

// Test 1: Basic state transition
static void testBasicTransition() {
    TEST("FSM: basic state transition");

    auto def = makeDef();
    Entity e(1, EntitySide::Cell, def);

    auto& fsm = e.fsm();
    fsm.addState("Idle");
    fsm.addState("Moving");
    fsm.addTransition("Idle", "Moving");

    bool ok = fsm.setState("Idle");
    ok = ok && fsm.isInState("Idle");
    ok = ok && fsm.state() == "Idle";

    ok = ok && fsm.setState("Moving");
    ok = ok && fsm.isInState("Moving");

    if (ok) PASS();
    else FAIL("state=" + fsm.state());
}

// Test 2: Blocked transition
static void testBlockedTransition() {
    TEST("FSM: blocked invalid transition");

    auto def = makeDef();
    Entity e(1, EntitySide::Cell, def);

    auto& fsm = e.fsm();
    fsm.addState("Idle");
    fsm.addState("Moving");
    fsm.addState("Combat");
    fsm.addTransition("Idle", "Moving");

    fsm.setState("Idle");
    fsm.setState("Moving");

    // Moving -> Combat not allowed
    bool ok = !fsm.setState("Combat");
    ok = ok && fsm.isInState("Moving");

    if (ok) PASS();
    else FAIL("should block Combat transition");
}

// Test 3: Enter/exit callbacks
static void testCallbacks() {
    TEST("FSM: enter/exit callbacks");

    auto def = makeDef();
    Entity e(1, EntitySide::Cell, def);

    auto& fsm = e.fsm();
    fsm.addState("Idle");
    fsm.addState("Combat");
    fsm.addTransition("Idle", "Combat");

    std::string enterOld, enterNew, exitOld, exitNew;
    fsm.setOnStateEnter([&](Entity&, const std::string& oldS, const std::string& newS) {
        enterOld = oldS;
        enterNew = newS;
    });
    fsm.setOnStateExit([&](Entity&, const std::string& oldS, const std::string& newS) {
        exitOld = oldS;
        exitNew = newS;
    });

    fsm.setState("Idle");
    enterOld.clear(); enterNew.clear(); exitOld.clear(); exitNew.clear();

    fsm.setState("Combat");

    bool ok = enterOld == "Idle" && enterNew == "Combat";
    ok = ok && exitOld == "Idle" && exitNew == "Combat";

    if (ok) PASS();
    else FAIL("enter=(" + enterOld + "," + enterNew + ") exit=(" + exitOld + "," + exitNew + ")");
}

// Test 4: Wildcard transitions
static void testWildcardTransition() {
    TEST("FSM: wildcard transition from any state");

    auto def = makeDef();
    Entity e(1, EntitySide::Cell, def);

    auto& fsm = e.fsm();
    fsm.addState("Idle");
    fsm.addState("Moving");
    fsm.addState("Combat");
    fsm.addState("Dead");
    fsm.addTransition("Idle", "Moving");
    fsm.addTransition("Moving", "Combat");
    fsm.addTransitionFromAny("Dead");

    fsm.setState("Idle");
    bool ok = fsm.setState("Moving");
    ok = ok && fsm.isInState("Moving");

    // Moving -> Dead via wildcard
    ok = ok && fsm.setState("Dead");
    ok = ok && fsm.isInState("Dead");

    if (ok) PASS();
    else FAIL("wildcard failed");
}

// Test 5: canTransitionTo query
static void testCanTransitionTo() {
    TEST("FSM: canTransitionTo query");

    auto def = makeDef();
    Entity e(1, EntitySide::Cell, def);

    auto& fsm = e.fsm();
    fsm.addState("Idle");
    fsm.addState("Moving");
    fsm.addState("Combat");
    fsm.addTransition("Idle", "Moving");
    fsm.addTransition("Idle", "Combat");

    fsm.setState("Idle");

    bool ok = fsm.canTransitionTo("Moving");
    ok = ok && fsm.canTransitionTo("Combat");
    ok = ok && !fsm.canTransitionTo("Idle");  // no self-transition defined

    if (ok) PASS();
    else FAIL("transition query failed");
}

// Test 6: availableTransitions list
static void testAvailableTransitions() {
    TEST("FSM: availableTransitions list");

    auto def = makeDef();
    Entity e(1, EntitySide::Cell, def);

    auto& fsm = e.fsm();
    fsm.addState("Idle");
    fsm.addState("Moving");
    fsm.addState("Combat");
    fsm.addState("Dead");
    fsm.addTransition("Idle", "Moving");
    fsm.addTransition("Idle", "Combat");
    fsm.addTransitionFromAny("Dead");

    fsm.setState("Idle");
    auto avail = fsm.availableTransitions();

    bool ok = avail.size() == 3;  // Moving, Combat, Dead
    ok = ok && std::find(avail.begin(), avail.end(), "Moving") != avail.end();
    ok = ok && std::find(avail.begin(), avail.end(), "Combat") != avail.end();
    ok = ok && std::find(avail.begin(), avail.end(), "Dead") != avail.end();

    if (ok) PASS();
    else FAIL("available count=" + std::to_string(avail.size()));
}

// Test 7: Reset FSM
static void testReset() {
    TEST("FSM: reset clears state");

    auto def = makeDef();
    Entity e(1, EntitySide::Cell, def);

    auto& fsm = e.fsm();
    fsm.addState("Idle");
    fsm.setState("Idle");
    fsm.reset();

    bool ok = fsm.state().empty();
    ok = ok && !fsm.isInState("Idle");

    if (ok) PASS();
    else FAIL("reset failed");
}

// Test 8: Multiple entities with independent FSMs
static void testMultipleEntities() {
    TEST("FSM: independent FSMs per entity");

    auto def = makeDef();
    Entity e1(1, EntitySide::Cell, def);
    Entity e2(2, EntitySide::Cell, def);

    auto& fsm1 = e1.fsm();
    auto& fsm2 = e2.fsm();

    fsm1.addState("Idle"); fsm1.addState("Combat");
    fsm1.addTransition("Idle", "Combat");

    fsm2.addState("Idle"); fsm2.addState("Moving");
    fsm2.addTransition("Idle", "Moving");

    fsm1.setState("Idle");
    fsm2.setState("Idle");

    fsm1.setState("Combat");
    fsm2.setState("Moving");

    bool ok = fsm1.isInState("Combat") && fsm2.isInState("Moving");
    ok = ok && !fsm1.isInState("Moving") && !fsm2.isInState("Combat");

    if (ok) PASS();
    else FAIL("independent FSMs failed");
}

// Test 9: States list and hasState
static void testStatesQuery() {
    TEST("FSM: states() and hasState() query");

    auto def = makeDef();
    Entity e(1, EntitySide::Cell, def);

    auto& fsm = e.fsm();
    fsm.addState("Idle");
    fsm.addState("Moving");
    fsm.addState("Dead");

    bool ok = fsm.hasState("Idle") && fsm.hasState("Moving") && fsm.hasState("Dead");
    ok = ok && !fsm.hasState("Flying");

    auto states = fsm.states();
    ok = ok && states.size() == 3;

    if (ok) PASS();
    else FAIL("states query failed");
}

// Test 10: Circular transitions
static void testCircularTransitions() {
    TEST("FSM: circular transitions (Idle->Combat->Idle)");

    auto def = makeDef();
    Entity e(1, EntitySide::Cell, def);

    auto& fsm = e.fsm();
    fsm.addState("Idle");
    fsm.addState("Combat");
    fsm.addTransition("Idle", "Combat");
    fsm.addTransition("Combat", "Idle");

    fsm.setState("Idle");
    fsm.setState("Combat");
    bool ok = fsm.isInState("Combat");

    fsm.setState("Idle");
    ok = ok && fsm.isInState("Idle");

    // Do it again
    fsm.setState("Combat");
    ok = ok && fsm.isInState("Combat");

    if (ok) PASS();
    else FAIL("circular transition failed");
}

// Test 11: Callback sees correct entity
static void testCallbackEntity() {
    TEST("FSM: callback receives correct entity");

    auto def = makeDef();
    Entity e(42, EntitySide::Cell, def);

    auto& fsm = e.fsm();
    fsm.addState("Idle");
    fsm.addState("Active");
    fsm.addTransition("Idle", "Active");

    EntityId callbackEntityId = 0;
    fsm.setOnStateEnter([&](Entity& entity, const std::string&, const std::string&) {
        callbackEntityId = entity.id();
    });

    fsm.setState("Idle");
    callbackEntityId = 0;
    fsm.setState("Active");

    bool ok = callbackEntityId == 42;

    if (ok) PASS();
    else FAIL("callback entity id=" + std::to_string(callbackEntityId));
}

// Test 12: Duplicate state no-op
static void testSameStateTransition() {
    TEST("FSM: transition to current state is no-op");

    auto def = makeDef();
    Entity e(1, EntitySide::Cell, def);

    auto& fsm = e.fsm();
    fsm.addState("Idle");
    fsm.addState("Active");
    fsm.addTransition("Idle", "Active");

    int callbackCount = 0;
    fsm.setOnStateEnter([&](Entity&, const std::string&, const std::string&) {
        ++callbackCount;
    });

    fsm.setState("Idle");
    int countAfterFirst = callbackCount;

    // Set same state again
    fsm.setState("Idle");

    bool ok = (callbackCount == countAfterFirst);  // no new callback
    ok = ok && fsm.isInState("Idle");

    if (ok) PASS();
    else FAIL("callback count=" + std::to_string(callbackCount));
}

int main() {
    std::cout << "State machine tests:\n";

    testBasicTransition();
    testBlockedTransition();
    testCallbacks();
    testWildcardTransition();
    testCanTransitionTo();
    testAvailableTransitions();
    testReset();
    testMultipleEntities();
    testStatesQuery();
    testCircularTransitions();
    testCallbackEntity();
    testSameStateTransition();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
