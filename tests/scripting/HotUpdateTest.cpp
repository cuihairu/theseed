#include "theseed/scripting/HotUpdate.h"

#include <iostream>
#include <string>
#include <utility>

using theseed::scripting::ChangeType;
using theseed::scripting::DiffChange;
using theseed::scripting::DiffResult;
using theseed::scripting::HotUpdateLevel;
using theseed::scripting::HotUpdateManager;
using theseed::scripting::HotUpdateValidator;
using theseed::scripting::isAutoApprove;
using theseed::scripting::isForbidden;
using theseed::scripting::levelOf;

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
        std::cout << "FAIL: " << (msg) << "\n";                 \
        ++testsFailed;                                          \
    } while (0)

static DiffChange makeChange(ChangeType type, std::string entity, std::string key, std::string detail) {
    DiffChange c;
    c.type = type;
    c.entityName = std::move(entity);
    c.key = std::move(key);
    c.detail = std::move(detail);
    return c;
}

// 1. change type → level mapping (§4.1 forbidden/auto-approve)
static void test_change_type_classification() {
    TEST("test_change_type_classification");
    if (levelOf(ChangeType::ConfigValueChange) != HotUpdateLevel::L1_Config) { FAIL("ConfigValueChange"); return; }
    if (levelOf(ChangeType::ModifyScriptBody) != HotUpdateLevel::L2_Script) { FAIL("ModifyScriptBody"); return; }
    if (levelOf(ChangeType::ModifyTimerLogic) != HotUpdateLevel::L2_Script) { FAIL("ModifyTimerLogic"); return; }
    if (levelOf(ChangeType::AddPropertyDefault) != HotUpdateLevel::L3_Def) { FAIL("AddPropertyDefault"); return; }
    if (levelOf(ChangeType::AddEntityType) != HotUpdateLevel::L3_Def) { FAIL("AddEntityType"); return; }
    if (levelOf(ChangeType::ChangePropertyType) != HotUpdateLevel::L4_NeedRestart) { FAIL("ChangePropertyType"); return; }
    if (levelOf(ChangeType::RemoveProperty) != HotUpdateLevel::L4_NeedRestart) { FAIL("RemoveProperty"); return; }
    if (levelOf(ChangeType::ChangeMethodSignature) != HotUpdateLevel::L4_NeedRestart) { FAIL("ChangeMethodSignature"); return; }
    if (levelOf(ChangeType::ChangeExposedProtocol) != HotUpdateLevel::L4_NeedRestart) { FAIL("ChangeExposedProtocol"); return; }
    if (levelOf(ChangeType::ChangeSerialization) != HotUpdateLevel::L4_NeedRestart) { FAIL("ChangeSerialization"); return; }
    PASS();
}

// 2. helper predicates align with §4.1
static void test_helper_predicates() {
    TEST("test_helper_predicates");
    if (!isAutoApprove(ChangeType::ConfigValueChange)) { FAIL("ConfigValueChange should be auto"); return; }
    if (!isAutoApprove(ChangeType::ModifyScriptBody)) { FAIL("ModifyScriptBody should be auto"); return; }
    if (isAutoApprove(ChangeType::ModifyTimerLogic)) { FAIL("ModifyTimerLogic should not be auto"); return; }
    if (!isForbidden(ChangeType::RemoveProperty)) { FAIL("RemoveProperty should be forbidden"); return; }
    if (!isForbidden(ChangeType::ChangeMethodSignature)) { FAIL("ChangeMethodSignature should be forbidden"); return; }
    if (isForbidden(ChangeType::ModifyScriptBody)) { FAIL("ModifyScriptBody should not be forbidden"); return; }
    PASS();
}

// 3. empty diff → L1, no rejections
static void test_validate_empty_diff() {
    TEST("test_validate_empty_diff");
    DiffResult empty;
    HotUpdateValidator validator;
    auto result = validator.validate(empty);
    if (result.level != HotUpdateLevel::L1_Config) { FAIL("empty diff should be L1"); return; }
    if (!result.canHotUpdate()) { FAIL("empty diff should hot-update"); return; }
    if (!result.rejections.empty()) { FAIL("empty diff should have no rejections"); return; }
    if (!result.warnings.empty()) { FAIL("empty diff should have no warnings"); return; }
    PASS();
}

// 4. all L1 config changes → L1
static void test_validate_l1_only() {
    TEST("test_validate_l1_only");
    DiffResult diff;
    diff.changes.push_back(makeChange(ChangeType::ConfigValueChange, "", "exp_rate", "1.5"));
    diff.changes.push_back(makeChange(ChangeType::ConfigValueChange, "", "max_players", "1000"));
    HotUpdateValidator validator;
    auto result = validator.validate(diff);
    if (result.level != HotUpdateLevel::L1_Config) { FAIL("expected L1"); return; }
    if (!result.canHotUpdate()) { FAIL("L1 should hot-update"); return; }
    PASS();
}

// 5. L2 script body changes → L2, no warnings
static void test_validate_l2_script_body() {
    TEST("test_validate_l2_script_body");
    DiffResult diff;
    diff.changes.push_back(makeChange(ChangeType::ModifyScriptBody, "Avatar", "onAttack", "new body"));
    HotUpdateValidator validator;
    auto result = validator.validate(diff);
    if (result.level != HotUpdateLevel::L2_Script) { FAIL("expected L2"); return; }
    if (!result.warnings.empty()) { FAIL("script body should not warn"); return; }
    PASS();
}

// 6. L2 timer logic → L2 + warning
static void test_validate_l2_timer_warning() {
    TEST("test_validate_l2_timer_warning");
    DiffResult diff;
    diff.changes.push_back(makeChange(ChangeType::ModifyTimerLogic, "Avatar", "respawnTimer", ""));
    HotUpdateValidator validator;
    auto result = validator.validate(diff);
    if (result.level != HotUpdateLevel::L2_Script) { FAIL("expected L2"); return; }
    if (result.warnings.size() != 1) { FAIL("expected 1 warning"); return; }
    PASS();
}

// 7. forbidden change promotes to L4 and records rejection
static void test_validate_forbidden_promotes_to_l4() {
    TEST("test_validate_forbidden_promotes_to_l4");
    DiffResult diff;
    diff.changes.push_back(makeChange(ChangeType::ConfigValueChange, "", "k", "v"));
    diff.changes.push_back(makeChange(ChangeType::ChangePropertyType, "Avatar", "hp", "Float32→Int32"));
    HotUpdateValidator validator;
    auto result = validator.validate(diff);
    if (result.level != HotUpdateLevel::L4_NeedRestart) { FAIL("expected L4"); return; }
    if (result.canHotUpdate()) { FAIL("L4 should not hot-update"); return; }
    if (result.rejections.size() != 1) { FAIL("expected 1 rejection"); return; }
    PASS();
}

// 8. L3 def-level change is rejected (Phase 2 not implemented)
static void test_validate_l3_rejected() {
    TEST("test_validate_l3_rejected");
    DiffResult diff;
    diff.changes.push_back(makeChange(ChangeType::AddPropertyDefault, "Avatar", "stamina", "100"));
    HotUpdateValidator validator;
    auto result = validator.validate(diff);
    if (result.level != HotUpdateLevel::L4_NeedRestart) { FAIL("expected L4 escalation"); return; }
    if (!result.canHotUpdate()) { PASS(); return; }
    FAIL("L3 should not be hot-updatable in MVP");
}

// 9. apply L1 config writes to configStore and bumps version
static void test_apply_l1_config() {
    TEST("test_apply_l1_config");
    DiffResult diff;
    diff.changes.push_back(makeChange(ChangeType::ConfigValueChange, "", "exp_rate", "1.5"));
    diff.changes.push_back(makeChange(ChangeType::ConfigValueChange, "", "max_players", "1000"));
    HotUpdateManager mgr;
    auto r = mgr.apply(diff);
    if (!r.success) { FAIL(r.message); return; }
    if (mgr.currentVersion() != "v1") { FAIL("expected version v1, got " + mgr.currentVersion()); return; }
    if (mgr.configStore().at("exp_rate") != "1.5") { FAIL("exp_rate not set"); return; }
    if (mgr.configStore().at("max_players") != "1000") { FAIL("max_players not set"); return; }
    PASS();
}

// 10. apply L2 writes to scriptStore
static void test_apply_l2_script() {
    TEST("test_apply_l2_script");
    DiffResult diff;
    diff.changes.push_back(makeChange(ChangeType::ModifyScriptBody, "Avatar", "onAttack", "return damage * 2"));
    HotUpdateManager mgr;
    auto r = mgr.apply(diff);
    if (!r.success) { FAIL(r.message); return; }
    if (mgr.scriptStore().at("onAttack") != "return damage * 2") { FAIL("script body not set"); return; }
    PASS();
}

// 11. apply forbidden diff fails without modifying state
static void test_apply_forbidden_does_not_modify() {
    TEST("test_apply_forbidden_does_not_modify");
    DiffResult diff;
    diff.changes.push_back(makeChange(ChangeType::RemoveProperty, "Avatar", "hp", ""));
    HotUpdateManager mgr;
    auto r = mgr.apply(diff);
    if (r.success) { FAIL("should reject"); return; }
    if (mgr.currentVersion() != "v0") { FAIL("version should stay v0"); return; }
    if (!mgr.scriptStore().empty()) { FAIL("scriptStore should be empty"); return; }
    PASS();
}

// 12. rollback restores previous snapshot
static void test_rollback_restores_previous() {
    TEST("test_rollback_restores_previous");
    HotUpdateManager mgr;
    DiffResult d1;
    d1.changes.push_back(makeChange(ChangeType::ConfigValueChange, "", "k", "v1"));
    mgr.apply(d1);

    DiffResult d2;
    d2.changes.push_back(makeChange(ChangeType::ConfigValueChange, "", "k", "v2"));
    mgr.apply(d2);
    if (mgr.configStore().at("k") != "v2") { FAIL("expected v2 after second apply"); return; }

    auto r = mgr.rollback();
    if (!r.success) { FAIL(r.message); return; }
    if (mgr.currentVersion() != "v1") { FAIL("expected rollback to v1"); return; }
    if (mgr.configStore().at("k") != "v1") { FAIL("expected k=v1 after rollback"); return; }
    PASS();
}

// 13. rollback to specific version
static void test_rollback_to_specific_version() {
    TEST("test_rollback_to_specific_version");
    HotUpdateManager mgr;
    for (int i = 1; i <= 3; ++i) {
        DiffResult d;
        d.changes.push_back(makeChange(ChangeType::ConfigValueChange, "", "k", "v" + std::to_string(i)));
        mgr.apply(d);
    }
    auto r = mgr.rollback("v1");
    if (!r.success) { FAIL(r.message); return; }
    if (mgr.currentVersion() != "v1") { FAIL("expected v1"); return; }
    if (mgr.configStore().at("k") != "v1") { FAIL("expected k=v1"); return; }
    PASS();
}

// 14. rollback with no history fails
static void test_rollback_no_history() {
    TEST("test_rollback_no_history");
    HotUpdateManager mgr;
    auto r = mgr.rollback();
    if (r.success) { FAIL("rollback with no history should fail"); return; }
    PASS();
}

// 15. rollback to unknown version fails
static void test_rollback_unknown_version() {
    TEST("test_rollback_unknown_version");
    HotUpdateManager mgr;
    DiffResult d;
    d.changes.push_back(makeChange(ChangeType::ConfigValueChange, "", "k", "v"));
    mgr.apply(d);
    auto r = mgr.rollback("v999");
    if (r.success) { FAIL("rollback to unknown should fail"); return; }
    PASS();
}

// 16. mixed L1 + L2 promoted to L2, both stores written on apply
static void test_apply_mixed_l1_l2() {
    TEST("test_apply_mixed_l1_l2");
    DiffResult diff;
    diff.changes.push_back(makeChange(ChangeType::ConfigValueChange, "", "exp_rate", "2.0"));
    diff.changes.push_back(makeChange(ChangeType::ModifyScriptBody, "Avatar", "onRespawn", "apply buff"));
    HotUpdateManager mgr;
    auto r = mgr.apply(diff);
    if (!r.success) { FAIL(r.message); return; }
    if (!mgr.configStore().count("exp_rate")) { FAIL("config missing"); return; }
    if (!mgr.scriptStore().count("onRespawn")) { FAIL("script missing"); return; }
    PASS();
}

// 17. appliedVersions reflects history
static void test_applied_versions_history() {
    TEST("test_applied_versions_history");
    HotUpdateManager mgr;
    DiffResult d;
    d.changes.push_back(makeChange(ChangeType::ConfigValueChange, "", "k", "v"));
    mgr.apply(d);
    mgr.apply(d);
    auto versions = mgr.appliedVersions();
    if (versions.size() != 3) { FAIL("expected 3 entries (v0 base + 2 applies)"); return; }
    if (versions[0] != "v0" || versions[1] != "v1" || versions[2] != "v2") { FAIL("version sequence wrong"); return; }
    PASS();
}

// 18. apply with timer-logic change reports warning count in message
static void test_apply_reports_warning_count() {
    TEST("test_apply_reports_warning_count");
    DiffResult diff;
    diff.changes.push_back(makeChange(ChangeType::ModifyTimerLogic, "Avatar", "respawnTimer", ""));
    HotUpdateManager mgr;
    auto r = mgr.apply(diff);
    if (!r.success) { FAIL(r.message); return; }
    if (r.message.find("1 warning") == std::string::npos) {
        FAIL("expected warning count in message: " + r.message);
        return;
    }
    PASS();
}

int main() {
    test_change_type_classification();
    test_helper_predicates();
    test_validate_empty_diff();
    test_validate_l1_only();
    test_validate_l2_script_body();
    test_validate_l2_timer_warning();
    test_validate_forbidden_promotes_to_l4();
    test_validate_l3_rejected();
    test_apply_l1_config();
    test_apply_l2_script();
    test_apply_forbidden_does_not_modify();
    test_rollback_restores_previous();
    test_rollback_to_specific_version();
    test_rollback_no_history();
    test_rollback_unknown_version();
    test_apply_mixed_l1_l2();
    test_applied_versions_history();

    std::cout << "  passed=" << testsPassed << " failed=" << testsFailed << "\n";
    return testsFailed == 0 ? 0 : 1;
}
