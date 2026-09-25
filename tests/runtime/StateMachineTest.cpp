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

// Test 10: availableTransitions with no current state returns all states
static void testAvailableTransitionsWithoutState() {
    TEST("FSM: availableTransitions before setState lists every state");

    auto def = makeDef();
    Entity e(1, EntitySide::Cell, def);

    auto& fsm = e.fsm();
    fsm.addState("Idle");
    fsm.addState("Moving");
    fsm.addState("Dead");

    // 尚未 setState：currentState_ 为空，返回全部已注册状态
    auto all = fsm.availableTransitions();
    bool ok = all.size() == 3;

    fsm.setState("Idle");
    fsm.addTransition("Idle", "Moving");
    auto fromIdle = fsm.availableTransitions();
    ok = ok && fromIdle.size() == 1 && fromIdle[0] == "Moving";

    if (ok) PASS();
    else FAIL("availableTransitions mismatch");
}

// 分支覆盖：addState/addTransition/addTransitionFromAny/setState 的防御早退臂
static void testDefensiveExits() {
    TEST("FSM: defensive early-exit arms");

    auto def = makeDef();
    Entity e(1, EntitySide::Cell, def);
    auto& fsm = e.fsm();

    bool ok = !fsm.addState("");                      // 空状态名拒绝
    ok = ok && !fsm.addTransition("", "Idle");        // 空 from 拒绝
    ok = ok && !fsm.addTransition("Idle", "");        // 空 to 拒绝
    ok = ok && !fsm.addTransition("", "");            // 双空拒绝
    ok = ok && !fsm.addTransitionFromAny("");         // 空 wildcard 目标拒绝

    // 未注册状态的转换拒绝（contains 检查两臂）
    ok = ok && !fsm.addTransition("Ghost", "Idle");   // from 未注册
    ok = ok && fsm.addState("Idle") && fsm.addState("Moving");
    ok = ok && !fsm.addTransition("Ghost", "Idle");   // from 仍未注册
    ok = ok && !fsm.addTransition("Idle", "Ghost");   // to 未注册
    ok = ok && !fsm.addTransitionFromAny("Ghost");    // wildcard 目标未注册

    // setState 防御：空名 / 未注册
    ok = ok && !fsm.setState("");
    ok = ok && !fsm.setState("Ghost");

    if (ok) PASS();
    else FAIL("defensive exits mismatch");
}

// 分支覆盖：availableTransitions 的 wildcard 去重与自排除臂
static void testWildcardDedupeExits() {
    TEST("FSM: wildcard dedupe and self-exclude arms");

    auto def = makeDef();
    Entity e(1, EntitySide::Cell, def);
    auto& fsm = e.fsm();

    fsm.addState("Idle");
    fsm.addState("Moving");
    fsm.addTransition("Idle", "Moving");
    // wildcard 目标与显式 transition 重复：不得二次入列（去重臂）
    fsm.addTransitionFromAny("Moving");
    // wildcard 目标等于当前状态：不得入列（自排除臂）
    fsm.addTransitionFromAny("Idle");

    fsm.setState("Idle");
    auto avail = fsm.availableTransitions();

    bool ok = avail.size() == 1 && avail[0] == "Moving";

    // 当前状态无显式出边、且 wildcard 目标全等于当前态时：结果为空
    Entity e2(2, EntitySide::Cell, def);
    auto& fsm2 = e2.fsm();
    fsm2.addState("Only");
    fsm2.addTransitionFromAny("Only");  // wildcard 目标 == 唯一状态
    fsm2.setState("Only");
    auto alone = fsm2.availableTransitions();  // 自排除后为空
    ok = ok && alone.empty();

    if (ok) PASS();
    else FAIL("wildcard dedupe mismatch");
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
    testAvailableTransitionsWithoutState();
    testStatesQuery();
    testCircularTransitions();
    testCallbackEntity();
    testSameStateTransition();
    testDefensiveExits();
    testWildcardDedupeExits();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
