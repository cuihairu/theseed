// Entity.h 模板族 / Entity.cpp 防御臂 / EntityCall / EntityRef 的分支覆盖补测。
// 每个 miss 边补一个场景；既有测试只覆盖了各短路链的正常臂，
// 这里把早退臂逐组合打全（聚合口径下一条场景清一个 miss 条目）。
#include "theseed/runtime/Controller.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityCall.h"
#include "theseed/runtime/EntityDef.h"
#include "theseed/runtime/EntityRef.h"
#include "theseed/runtime/PropertyBlock.h"
#include "theseed/runtime/RuntimeTransport.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

using theseed::runtime::DeliveryClass;
using theseed::runtime::Entity;
using theseed::runtime::EntityCall;
using theseed::runtime::EntityDef;
using theseed::runtime::EntityId;
using theseed::runtime::EntityRef;
using theseed::runtime::EntitySide;
using theseed::runtime::EntityState;
using theseed::runtime::InMemoryRuntimeTransport;
using theseed::runtime::MethodSide;
using theseed::runtime::PropertyType;
using theseed::runtime::SendResult;

namespace {

int fail(const char* stage) {
    std::cerr << "entity_template_branch_test_failed_at=" << stage << '\n';
    return EXIT_FAILURE;
}

std::vector<std::byte> i32Bytes(std::int32_t v) {
    std::vector<std::byte> buf(4);
    std::memcpy(buf.data(), &v, 4);
    return buf;
}

std::vector<std::byte> f32Bytes(float v) {
    std::vector<std::byte> buf(4);
    std::memcpy(buf.data(), &v, 4);
    return buf;
}

EntityDef makeDef() {
    EntityDef def("Avatar");
    // hp: min+max 全约束；speed: 仅 min；ratio: 仅 max；plain: 无约束；
    // wide: Int64（onPropertyChanged 尺寸不符场景）；nick: String。
    def.addProperty("hp", PropertyType::Int32, sizeof(std::int32_t), {},
                    {}, i32Bytes(0), i32Bytes(100));
    def.addProperty("mana", PropertyType::UInt32, sizeof(std::uint32_t));
    def.addProperty("speed", PropertyType::Float32, sizeof(float), {},
                    {}, f32Bytes(0.0F), {});
    def.addProperty("ratio", PropertyType::Float32, sizeof(float), {},
                    {}, {}, f32Bytes(1.0F));
    def.addProperty("plain", PropertyType::Float32, sizeof(float));
    def.addProperty("wide", PropertyType::Int64, sizeof(std::int64_t));
    def.addProperty("noMin", PropertyType::Int32, sizeof(std::int32_t));

    // Cell 方法（覆盖 bindTypedMethodHandler 既有测试用过的全部 Args 组合）
    def.addMethod("castSpell", MethodSide::Cell, {{"dmg", PropertyType::Int32}});
    def.addMethod("heal", MethodSide::Cell, {{"amount", PropertyType::Float32}});
    def.addMethod("moveTo", MethodSide::Cell,
                  {{"x", PropertyType::Float32}, {"y", PropertyType::Float32}});
    def.addMethod("takeDamage", MethodSide::Cell,
                  {{"amount", PropertyType::Float32}, {"src", PropertyType::Int32}});
    def.addMethod("castSkill", MethodSide::Cell, {{"id", PropertyType::Int32}});
    def.addMethod("equipArmor", MethodSide::Cell, {{"id", PropertyType::Int32}});
    def.addMethod("complex", MethodSide::Cell,
                  {{"a", PropertyType::Int32}, {"b", PropertyType::Float32},
                   {"c", PropertyType::String}});
    // Base 方法
    def.addMethod("basePing", MethodSide::Base, {{"tag", PropertyType::String}});
    def.addMethod("buyItem", MethodSide::Base,
                  {{"item", PropertyType::Int32}, {"count", PropertyType::Int32}});
    def.addMethod("addScore", MethodSide::Base,
                  {{"reason", PropertyType::String}, {"score", PropertyType::Int32}});
    // Client 方法（callDefMethod switch 的 default 臂）
    def.addMethod("announce", MethodSide::Client, {{"text", PropertyType::String}});
    return def;
}

// 场景 A/B：findProperty / setProperty 按名字取值，无效名早退
bool testNameLookupExits(Entity& e) {
    if (e.findProperty<std::int32_t>("nope") != nullptr) return false;
    if (e.findProperty<float>("nope") != nullptr) return false;
    if (e.findProperty<std::int64_t>("nope") != nullptr) return false;
    // int64 实例的命中臂（wide 存在）
    if (e.findProperty<std::int64_t>("wide") == nullptr) return false;
    if (e.setProperty<std::int32_t>("nope", 1)) return false;
    if (e.setProperty<float>("nope", 1.0F)) return false;
    // int64 实例的无效名早退臂
    if (e.setProperty<std::int64_t>("nope", 1LL)) return false;
    return true;
}

// 场景 C：validateProperty 全约束组合（无效名 / 仅 min / 仅 max / 双空 / 双有）
bool testValidateMatrix(Entity& e) {
    if (e.validateProperty<std::int32_t>("nope", 1)) return false;
    if (e.validateProperty<float>("nope", 1.0F)) return false;
    // 仅 min：低于 min 拒；min 之上无 max（maxValue 为空跳过检查）通过
    if (e.validateProperty<float>("speed", -1.0F)) return false;
    if (!e.validateProperty<float>("speed", 999.0F)) return false;
    // 仅 max：超 max 拒；max 之下无 min（minValue 为空跳过检查）通过
    if (e.validateProperty<float>("ratio", 2.0F)) return false;
    if (!e.validateProperty<float>("ratio", -100.0F)) return false;
    // 双空：恒通过
    if (!e.validateProperty<float>("plain", 42.0F)) return false;
    // Int32 实例的 min 空臂（noMin 无约束）
    if (!e.validateProperty<std::int32_t>("noMin", -7)) return false;
    // 双有：边界内外
    if (!e.validateProperty<std::int32_t>("hp", 50)) return false;
    if (e.validateProperty<std::int32_t>("hp", -1)) return false;
    if (e.validateProperty<std::int32_t>("hp", 101)) return false;
    return true;
}

// 场景 D：clampProperty 全约束组合
bool testClampMatrix(Entity& e) {
    if (e.clampProperty<std::int32_t>("nope", 7) != 7) return false;
    if (e.clampProperty<float>("nope", 7.0F) != 7.0F) return false;
    // 仅 min：低于 min 夹到 min；高于 min 原样（无 max，跳过 max 夹取）
    if (e.clampProperty<float>("speed", -5.0F) != 0.0F) return false;
    if (e.clampProperty<float>("speed", 88.0F) != 88.0F) return false;
    // 仅 max：超 max 夹到 max；低于 max 原样（无 min，跳过 min 夹取）
    if (e.clampProperty<float>("ratio", 5.0F) != 1.0F) return false;
    if (e.clampProperty<float>("ratio", -5.0F) != -5.0F) return false;
    // 双空：原样
    if (e.clampProperty<float>("plain", 3.0F) != 3.0F) return false;
    // 双有：三区间
    if (e.clampProperty<std::int32_t>("hp", -3) != 0) return false;
    if (e.clampProperty<std::int32_t>("hp", 50) != 50) return false;
    if (e.clampProperty<std::int32_t>("hp", 500) != 100) return false;
    return true;
}

// 场景 E：onPropertyChanged 回调载荷尺寸不符早退 + 正常触发
bool testPropertyChangedSizeGate(Entity& e) {
    int wideCalls = 0;
    // wide 是 Int64（8 字节），用 int32_t 包装：回调收 size=8 != sizeof(T) → 早退
    e.onPropertyChanged<std::int32_t>(
        "wide", [&](Entity&, std::int32_t, std::int32_t) { ++wideCalls; });
    e.setProperty<std::int64_t>("wide", 123456789LL);
    if (wideCalls != 0) return false;  // 尺寸闸拦截，回调不得执行

    int hpCalls = 0;
    e.onPropertyChanged<std::int32_t>(
        "hp", [&](Entity&, std::int32_t, std::int32_t) { ++hpCalls; });
    e.setProperty<std::int32_t>("hp", 66);
    if (hpCalls != 1) return false;

    // float 实例的尺寸闸双臂：wide(8B) 对 float(4B) 早退、speed(4B) 匹配执行
    int fMismatch = 0, fMatch = 0;
    e.onPropertyChanged<float>(
        "wide", [&](Entity&, float, float) { ++fMismatch; });
    e.onPropertyChanged<float>(
        "speed", [&](Entity&, float, float) { ++fMatch; });
    e.setProperty<std::int64_t>("wide", 2LL);
    if (fMismatch != 0) return false;
    e.setProperty<float>("speed", 5.0F);
    if (fMatch != 1) return false;

    // 按名字绑定的重载：无效名 → false
    if (e.onPropertyChanged<std::int32_t>(
            "nope", [&](Entity&, std::int32_t, std::int32_t) {})) {
        return false;
    }
    return true;
}

}  // namespace

int main() {
    const auto def = makeDef();

    // --- 属性名查找 / 校验 / 夹取矩阵（Cell 实体） ---
    {
        Entity e(1, EntitySide::Cell, def);
        e.activate();
        if (!testNameLookupExits(e)) return fail("name_lookup_exits");
        if (!testValidateMatrix(e)) return fail("validate_matrix");
        if (!testClampMatrix(e)) return fail("clamp_matrix");
        if (!testPropertyChangedSizeGate(e)) return fail("prop_changed_size_gate");
    }

    // --- callCellWith / callBaseWith / callDefMethod 的连接状态矩阵 ---
    {
        InMemoryRuntimeTransport transport;
        Entity cell(10, EntitySide::Cell, def);
        cell.activate();

        // 无 transport：NotConnected（!transport_ 臂）
        if (cell.callCellWith<std::int32_t>("castSpell", 1) != SendResult::NotConnected) {
            return fail("cellwith_no_transport");
        }
        if (cell.callCellWith<float, std::int32_t>("takeDamage", 1.0F, 2)
            != SendResult::NotConnected) {
            return fail("cellwith2_no_transport");
        }
        if (cell.callCellWith<std::int32_t, float, std::string>("complex", 1, 2.0F, "x")
            != SendResult::NotConnected) {
            return fail("cellwith3_no_transport");
        }

        // 有 transport 但未 bindCell：NotConnected（!cellCall_ 臂）
        cell.setTransport(&transport);
        if (cell.callCellWith<std::int32_t>("castSpell", 1) != SendResult::NotConnected) {
            return fail("cellwith_no_cellcall");
        }
        if (cell.callCellWith<float, std::int32_t>("takeDamage", 1.0F, 2)
            != SendResult::NotConnected) {
            return fail("cellwith2_no_cellcall");
        }
        if (cell.callCellWith<std::int32_t, float, std::string>("complex", 1, 2.0F, "x")
            != SendResult::NotConnected) {
            return fail("cellwith3_no_cellcall");
        }

        // bind 后正常发送
        cell.bindCellEntityCall(22, DeliveryClass::UNORDERED_LOSSY);
        if (cell.callCellWith<std::int32_t>("castSpell", 1) != SendResult::Accepted) {
            return fail("cellwith_ok");
        }
        if (cell.callCellWith<float, std::int32_t>("takeDamage", 1.0F, 2)
            != SendResult::Accepted) {
            return fail("cellwith2_ok");
        }
        if (cell.callCellWith<std::int32_t, float, std::string>("complex", 1, 2.0F, "x")
            != SendResult::Accepted) {
            return fail("cellwith3_ok");
        }

        // invalidate 后：NotConnected（!isValid() 臂）
        cell.cellEntityCall()->invalidate();
        if (cell.callCellWith<std::int32_t>("castSpell", 1) != SendResult::NotConnected) {
            return fail("cellwith_invalid");
        }
        if (cell.callCellWith<float, std::int32_t>("takeDamage", 1.0F, 2)
            != SendResult::NotConnected) {
            return fail("cellwith2_invalid");
        }
        if (cell.callCellWith<std::int32_t, float, std::string>("complex", 1, 2.0F, "x")
            != SendResult::NotConnected) {
            return fail("cellwith3_invalid");
        }
        cell.cellEntityCall()->updateTarget(22);  // 恢复，供 callDefMethod 用

        // callBaseWith（Base 实体 + baseCall 同矩阵）
        Entity base(11, EntitySide::Base, def);
        base.activate();
        if (base.callBaseWith<std::string, std::int32_t>("addScore", "kill", 10)
            != SendResult::NotConnected) {
            return fail("basewith_no_transport");
        }
        base.setTransport(&transport);
        if (base.callBaseWith<std::string, std::int32_t>("addScore", "kill", 10)
            != SendResult::NotConnected) {
            return fail("basewith_no_basecall");
        }
        base.bindBaseEntityCall(21);
        if (base.callBaseWith<std::string, std::int32_t>("addScore", "kill", 10)
            != SendResult::Accepted) {
            return fail("basewith_ok");
        }
        base.baseEntityCall()->invalidate();
        if (base.callBaseWith<std::string, std::int32_t>("addScore", "kill", 10)
            != SendResult::NotConnected) {
            return fail("basewith_invalid");
        }

        // callDefMethod：未知方法 / Cell 侧 / Base 侧 / Client 侧（default 臂）
        if (cell.callDefMethod<float>("noSuchMethod", 1.0F) != SendResult::NotConnected) {
            return fail("defmethod_unknown");
        }
        // 两参实例的未知方法早退臂
        if (cell.callDefMethod<std::string, std::int32_t>("noSuchMethod", "x", 1)
            != SendResult::NotConnected) {
            return fail("defmethod_unknown2");
        }
        if (cell.callDefMethod<float>("heal", 5.0F) != SendResult::Accepted) {
            return fail("defmethod_cell");
        }
        // Cell 实体无 baseCall：Base 侧方法走 callBaseWith 早退
        if (cell.callDefMethod<std::string, std::int32_t>("addScore", "x", 1)
            != SendResult::NotConnected) {
            return fail("defmethod_base_no_route");
        }
        // Client 侧方法在实体上调用：switch default 臂
        if (cell.callDefMethod<std::string>("announce", "hi")
            != SendResult::NotConnected) {
            return fail("defmethod_client_default");
        }
        // Base 实体上 Base 侧正常
        base.bindBaseEntityCall(21);
        if (base.callDefMethod<std::string, std::int32_t>("addScore", "x", 1)
            != SendResult::Accepted) {
            return fail("defmethod_base_ok");
        }
    }

    // --- bindTypedMethodHandler：8 个既有 Args 组合 × 早退全家 ---
    // 模板参包显式给前缀后其余仍需从 handler 实参推导，lambda/nullptr
    // 无法引导 std::function 推导——沿用既有测试手法：具名 function 变量。
    {
        using F1f = std::function<void(Entity&, float)>;
        using F2f = std::function<void(Entity&, float, float)>;
        using Ffi = std::function<void(Entity&, float, std::int32_t)>;
        using Fi = std::function<void(Entity&, std::int32_t)>;
        using Fifs = std::function<void(Entity&, std::int32_t, float, std::string)>;
        using Fii = std::function<void(Entity&, std::int32_t, std::int32_t)>;
        using Fsi = std::function<void(Entity&, std::string, std::int32_t)>;

        Entity cell(20, EntitySide::Cell, def);
        cell.activate();
        Entity base(21, EntitySide::Base, def);
        base.activate();

        const F1f f1f = [](Entity&, float) {};
        const F2f f2f = [](Entity&, float, float) {};
        const Ffi ffi = [](Entity&, float, std::int32_t) {};
        const Fi fi = [](Entity&, std::int32_t) {};
        const Fifs fifs = [](Entity&, std::int32_t, float, std::string) {};
        const Fii fii = [](Entity&, std::int32_t, std::int32_t) {};
        const Fsi fsi = [](Entity&, std::string, std::int32_t) {};

        // L320 短路链：空名（真）/ 空 handler（第二真）
        if (cell.bindTypedMethodHandler<float>("", f1f)) return fail("bt_heal_empty");
        if (cell.bindTypedMethodHandler<float>("heal", F1f{})) return fail("bt_heal_null");
        if (cell.bindTypedMethodHandler<float, float>("", f2f)) return fail("bt_move_empty");
        if (cell.bindTypedMethodHandler<float, float>("moveTo", F2f{})) return fail("bt_move_null");
        if (cell.bindTypedMethodHandler<float, std::int32_t>("", ffi)) return fail("bt_dmg_empty");
        if (cell.bindTypedMethodHandler<float, std::int32_t>("takeDamage", Ffi{})) {
            return fail("bt_dmg_null");
        }
        if (cell.bindTypedMethodHandler<std::int32_t>("", fi)) return fail("bt_skill_empty");
        if (cell.bindTypedMethodHandler<std::int32_t>("castSkill", Fi{})) {
            return fail("bt_skill_null");
        }
        if (cell.bindTypedMethodHandler<std::int32_t, float, std::string>("", fifs)) {
            return fail("bt_cplx_empty");
        }
        if (cell.bindTypedMethodHandler<std::int32_t, float, std::string>("complex", Fifs{})) {
            return fail("bt_cplx_null");
        }
        if (base.bindTypedMethodHandler<std::int32_t, std::int32_t>("", fii)) {
            return fail("bt_buy_empty");
        }
        if (base.bindTypedMethodHandler<std::int32_t, std::int32_t>("buyItem", Fii{})) {
            return fail("bt_buy_null");
        }
        if (base.bindTypedMethodHandler<std::string, std::int32_t>("", fsi)) {
            return fail("bt_score_empty");
        }
        if (base.bindTypedMethodHandler<std::string, std::int32_t>("addScore", Fsi{})) {
            return fail("bt_score_null");
        }

        // L323：未知方法（!descriptor 真）
        if (cell.bindTypedMethodHandler<float>("noSuchMethod", f1f)) {
            return fail("bt_heal_unknown");
        }
        if (cell.bindTypedMethodHandler<std::int32_t>("noSuchMethod", fi)) {
            return fail("bt_skill_unknown");
        }
        if (base.bindTypedMethodHandler<std::string, std::int32_t>("noSuchMethod", fsi)) {
            return fail("bt_score_unknown");
        }

        // L323：side 不符（!supportsMethodSide 真）
        if (base.bindTypedMethodHandler<float, float>("moveTo", f2f)) return fail("bt_move_side");
        if (base.bindTypedMethodHandler<float, std::int32_t>("takeDamage", ffi)) {
            return fail("bt_dmg_side");
        }
        if (base.bindTypedMethodHandler<std::int32_t>("castSkill", fi)) {
            return fail("bt_skill_side");
        }
        if (base.bindTypedMethodHandler<std::int32_t, float, std::string>("complex", fifs)) {
            return fail("bt_cplx_side");
        }
        if (cell.bindTypedMethodHandler<std::int32_t, std::int32_t>("buyItem", fii)) {
            return fail("bt_buy_side");
        }
        if (cell.bindTypedMethodHandler<std::string, std::int32_t>("addScore", fsi)) {
            return fail("bt_score_side");
        }

        // 正常绑定 + 触发（各组合至少一次成功绑定）
        int hits = 0;
        F1f healFn = [&](Entity&, float) { ++hits; };
        Ffi dmgFn = [&](Entity&, float, std::int32_t) { ++hits; };
        Fsi scoreFn = [&](Entity&, std::string, std::int32_t) { ++hits; };
        if (!cell.bindTypedMethodHandler<float>("heal", std::move(healFn))) {
            return fail("bt_heal_bind");
        }
        if (!cell.bindTypedMethodHandler<float, std::int32_t>("takeDamage", std::move(dmgFn))) {
            return fail("bt_dmg_bind");
        }
        if (!base.bindTypedMethodHandler<std::string, std::int32_t>("addScore",
                                                                    std::move(scoreFn))) {
            return fail("bt_score_bind");
        }
        std::vector<std::byte> payload(sizeof(float));
        const float amount = 2.5F;
        std::memcpy(payload.data(), &amount, sizeof(amount));
        if (!cell.dispatchMethod("heal", payload)) return fail("bt_heal_dispatch");
        if (hits != 1) return fail("bt_dispatch_hits");

        // bindStreamMethodHandler：未知方法（非空名 + side 相符）早退
        if (cell.bindStreamMethodHandler("noSuchStream",
                                         [](Entity&, theseed::foundation::MemoryStream&) {})) {
            return fail("stream_unknown");
        }

        // dispatchMethod side 不符：Cell 实体派发 Base 方法
        if (cell.dispatchMethod("basePing", {})) return fail("dispatch_side_mismatch");
    }

    // --- subscribe 早退 / notifyControllerComplete 无回调 / cancelController 惰性 ---
    {
        Entity e(30, EntitySide::Cell, def);
        e.activate();

        e.subscribe("", [](Entity&, std::string_view, std::span<const std::byte>) {});
        e.subscribe("boom", {});
        e.subscribe("boom", [](Entity&, std::string_view, std::span<const std::byte>) {});
        e.unsubscribe("boom");

        // controllers 未初始化（未触达 controllers() 访问器前）：安全 no-op
        e.cancelController(0);
        static_cast<void>(e.controllers().count());  // 之后访问仍正常
        // 无完成回调：notify no-op
        e.notifyControllerComplete(1, 2, true);

        // 定时器：注入调度函数后 null callback 仍早退（!callback 臂）
        bool scheduled = false;
        e.setTimerScheduleFns(
            [&](theseed::runtime::Duration, Entity::EntityTimerCallback cb) {
                scheduled = true;
                cb(e);  // 立即执行验证 fn 真被调用
                return theseed::foundation::TimerHandle{77, 0};
            },
            [&](theseed::runtime::Duration, Entity::EntityTimerCallback) {
                return theseed::foundation::TimerHandle{78, 0};
            });
        if (e.addTimer(std::chrono::milliseconds(5), nullptr)) {
            return fail("timer_null_cb");
        }
        if (e.addPeriodicTimer(std::chrono::milliseconds(5), nullptr)) {
            return fail("periodic_null_cb");
        }
        // 正常调度路径
        const auto h = e.addTimer(std::chrono::milliseconds(5), [](Entity&) {});
        if (!scheduled || h.id != 77) return fail("timer_schedule_fn");
    }

    // --- callCell/callBase 非模板版本的连接矩阵 + Migrating destroy ---
    {
        InMemoryRuntimeTransport transport;
        Entity e(40, EntitySide::Cell, def);
        e.activate();
        // 无 transport
        if (e.callCell("castSpell") != SendResult::NotConnected) return fail("callcell_nt");
        if (e.callBase("basePing") != SendResult::NotConnected) return fail("callbase_nt");
        // 有 transport 无 bind
        e.setTransport(&transport);
        if (e.callCell("castSpell") != SendResult::NotConnected) return fail("callcell_nocell");
        if (e.callBase("basePing") != SendResult::NotConnected) return fail("callbase_nobase");
        // bind 后 invalidate
        e.bindCellEntityCall(22);
        e.cellEntityCall()->invalidate();
        if (e.callCell("castSpell") != SendResult::NotConnected) return fail("callcell_invalid");

        // Migrating 状态直接 destroy（destroy 条件的第三臂）
        Entity m(41, EntitySide::Cell, def);
        m.activate();
        m.beginMigration();
        m.destroy();
        if (m.state() != EntityState::Destroyed) return fail("migrating_destroy");
    }

    // --- EntityCall::call 早退（Closed）与正常发送 ---
    {
        InMemoryRuntimeTransport transport;
        EntityCall invalid;
        if (invalid.call(transport, "m", {}) != SendResult::Closed) {
            return fail("call_invalid_closed");
        }
        EntityCall call(1, 2, "Avatar");
        if (call.call(transport, "", {}) != SendResult::Closed) {
            return fail("call_empty_method");
        }
        if (call.call(transport, "m", {}) != SendResult::Accepted) {
            return fail("call_ok");
        }
    }

    // --- EntityRef 三态 ---
    {
        Entity entity(50, EntitySide::Cell, def);
        entity.activate();

        EntityRef invalidRef = EntityRef::invalid();
        if (invalidRef.isValid()) return fail("ref_invalid");
        if (invalidRef.get() != nullptr) return fail("ref_invalid_get");

        EntityRef live = EntityRef::fromEntity(entity);
        if (!live.isValid() || live.get() != &entity) return fail("ref_live");
        if (live->id() != 50) return fail("ref_arrow");

        entity.destroy();
        if (live.isValid()) return fail("ref_destroyed");
        if (live.get() != nullptr) return fail("ref_destroyed_get");
    }

    std::cout << "entity_template_branch_test_ok\n";
    return EXIT_SUCCESS;
}
