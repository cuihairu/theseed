#include "theseed/codegen/CSharpEmitter.h"
#include "theseed/runtime/EntityDef.h"

#include <iostream>
#include <string>

using theseed::codegen::CSharpEmitOptions;
using theseed::codegen::CSharpEmitter;
using theseed::runtime::EntityDef;
using theseed::runtime::MethodSide;
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
        std::cout << "FAIL: " << (msg) << "\n";                 \
        ++testsFailed;                                          \
    } while (0)

#define ASSERT_CONTAINS(haystack, needle)                                       \
    do {                                                                        \
        const std::string& _h = (haystack);                                     \
        const std::string& _n = (needle);                                       \
        if (_h.find(_n) == std::string::npos) {                                 \
            FAIL("expected output to contain: " + _n);                           \
            return;                                                             \
        }                                                                       \
    } while (0)

#define ASSERT_NOT_CONTAINS(haystack, needle)                                   \
    do {                                                                        \
        const std::string& _h = (haystack);                                     \
        const std::string& _n = (needle);                                       \
        if (_h.find(_n) != std::string::npos) {                                 \
            FAIL("expected output to NOT contain: " + _n);                       \
            return;                                                             \
        }                                                                       \
    } while (0)

// 1. PropertyType → C# type mapping
static void test_csharp_type_mapping() {
    TEST("test_csharp_type_mapping");
    if (CSharpEmitter::csharpType(PropertyType::Int8) != "sbyte") { FAIL("Int8"); return; }
    if (CSharpEmitter::csharpType(PropertyType::Int32) != "int") { FAIL("Int32"); return; }
    if (CSharpEmitter::csharpType(PropertyType::UInt64) != "ulong") { FAIL("UInt64"); return; }
    if (CSharpEmitter::csharpType(PropertyType::Float32) != "float") { FAIL("Float32"); return; }
    if (CSharpEmitter::csharpType(PropertyType::Float64) != "double") { FAIL("Float64"); return; }
    if (CSharpEmitter::csharpType(PropertyType::Bool) != "bool") { FAIL("Bool"); return; }
    if (CSharpEmitter::csharpType(PropertyType::String) != "string") { FAIL("String"); return; }
    if (CSharpEmitter::csharpType(PropertyType::Vector3) != "UnityEngine.Vector3") { FAIL("Vector3"); return; }
    if (CSharpEmitter::csharpType(PropertyType::Blob) != "byte[]") { FAIL("Blob"); return; }
    PASS();
}

// 2. emit simple entity — namespace + class + Serialize/Deserialize skeleton
static void test_emit_simple_entity() {
    TEST("test_emit_simple_entity");
    EntityDef def("Avatar");
    def.addProperty("level", PropertyType::Int32, 0, PropertyFlag::None);
    auto [filename, content] = CSharpEmitter().emitEntity(def);
    if (filename != "Avatar.Generated.cs") { FAIL("filename"); return; }
    ASSERT_CONTAINS(content, "namespace Theseed.Generated");
    ASSERT_CONTAINS(content, "public sealed partial class Avatar : EntityBase");
    ASSERT_CONTAINS(content, "public int Level { get; set; }");
    ASSERT_CONTAINS(content, "public override void Serialize(BinaryWriter writer)");
    ASSERT_CONTAINS(content, "writer.Write(Level);");
    ASSERT_CONTAINS(content, "public override void Deserialize(BinaryReader reader)");
    ASSERT_CONTAINS(content, "Level = reader.ReadInt32();");
    PASS();
}

// 3. ClientSync flag → SyncProperty<T> wrapping
static void test_emit_client_sync_property() {
    TEST("test_emit_client_sync_property");
    EntityDef def("Avatar");
    def.addProperty("hp", PropertyType::Float32, 0, PropertyFlag::ClientSync);
    auto [_, content] = CSharpEmitter().emitEntity(def);
    ASSERT_CONTAINS(content, "public SyncProperty<float> Hp { get; } = new SyncProperty<float>(default);");
    ASSERT_CONTAINS(content, "writer.Write(Hp.current);");
    ASSERT_CONTAINS(content, "Hp.current = reader.ReadSingle();");
    ASSERT_CONTAINS(content, "Hp.NotifyChanged();");
    PASS();
}

// 4. side=Base / Cell methods → exposed Call_Xxx
static void test_emit_exposed_methods() {
    TEST("test_emit_exposed_methods");
    EntityDef def("Avatar");
    def.addMethod("attack", MethodSide::Cell, {});
    def.addMethod("respawn", MethodSide::Base, {});
    auto [_, content] = CSharpEmitter().emitEntity(def);
    ASSERT_CONTAINS(content, "public void Call_Attack()");
    ASSERT_CONTAINS(content, "InvokeServerMethod(0);");
    ASSERT_CONTAINS(content, "public void Call_Respawn()");
    ASSERT_CONTAINS(content, "InvokeServerMethod(1);");
    PASS();
}

// 5. side=Client methods → partial On_Xxx callback stub
static void test_emit_client_callback_method() {
    TEST("test_emit_client_callback_method");
    EntityDef def("Avatar");
    def.addMethod("onDamaged", MethodSide::Client,
                  {{"amount", PropertyType::Float32}, {"attackerId", PropertyType::UInt64}});
    auto [_, content] = CSharpEmitter().emitEntity(def);
    ASSERT_CONTAINS(content, "partial void On_OnDamaged(float amount, ulong attackerId);");
    PASS();
}

// 6. method args of all types are mapped
static void test_emit_method_with_args() {
    TEST("test_emit_method_with_args");
    EntityDef def("Avatar");
    def.addMethod("useItem", MethodSide::Base, {
        {"itemId", PropertyType::UInt32},
        {"count", PropertyType::Int16},
        {"name", PropertyType::String},
    });
    auto [_, content] = CSharpEmitter().emitEntity(def);
    ASSERT_CONTAINS(content, "public void Call_UseItem(uint itemId, short count, string name)");
    ASSERT_CONTAINS(content, "InvokeServerMethod(0, itemId, count, name);");
    PASS();
}

// 7. multi-entity registry generation
static void test_emit_registry() {
    TEST("test_emit_registry");
    EntityDef a("Avatar");
    EntityDef b("Npc");
    a.addProperty("hp", PropertyType::Float32);
    b.addProperty("name", PropertyType::String);
    std::vector<const EntityDef*> defs = { &a, &b };
    auto [filename, content] = CSharpEmitter().emitRegistry(defs);
    if (filename != "EntityRegistry.Generated.cs") { FAIL("filename"); return; }
    ASSERT_CONTAINS(content, "public const int EntityCount = 2;");
    ASSERT_CONTAINS(content, "\"Avatar\"");
    ASSERT_CONTAINS(content, "\"Npc\"");
    ASSERT_CONTAINS(content, "EntityNames = new string[]");
    PASS();
}

// 8. emit() produces one file per entity + registry
static void test_emit_aggregate() {
    TEST("test_emit_aggregate");
    EntityDef a("Avatar");
    EntityDef b("Npc");
    std::vector<const EntityDef*> defs = { &a, &b };
    auto files = CSharpEmitter().emit(defs);
    if (files.size() != 3) { FAIL("expected 3 files"); return; }
    if (files.count("Avatar.Generated.cs") != 1) { FAIL("Avatar file missing"); return; }
    if (files.count("Npc.Generated.cs") != 1) { FAIL("Npc file missing"); return; }
    if (files.count("EntityRegistry.Generated.cs") != 1) { FAIL("registry missing"); return; }
    PASS();
}

// 9. emitHeader=false suppresses the header banner
static void test_emit_no_header() {
    TEST("test_emit_no_header");
    CSharpEmitOptions opts;
    opts.emitHeader = false;
    CSharpEmitter emitter(opts);
    EntityDef def("Avatar");
    def.addProperty("hp", PropertyType::Float32);
    auto [_, content] = emitter.emitEntity(def);
    ASSERT_NOT_CONTAINS(content, "<auto-generated>");
    ASSERT_CONTAINS(content, "public sealed partial class Avatar");
    PASS();
}

// 10. snake_case → PascalCase in fields/methods
static void test_pascal_case_conversion() {
    TEST("test_pascal_case_conversion");
    EntityDef def("avatar");
    def.addProperty("current_hp", PropertyType::Float32);
    def.addMethod("on_player_die", MethodSide::Cell, {});
    auto [_, content] = CSharpEmitter().emitEntity(def);
    ASSERT_CONTAINS(content, "public sealed partial class Avatar");
    ASSERT_CONTAINS(content, "public float CurrentHp { get; set; }");
    ASSERT_CONTAINS(content, "public void Call_OnPlayerDie()");
    PASS();
}

// 11. Vector3 serialization reads .x/.y/.z; deserialization constructs Vector3
static void test_vector3_serialization() {
    TEST("test_vector3_serialization");
    EntityDef def("Avatar");
    def.addProperty("position", PropertyType::Vector3);
    auto [_, content] = CSharpEmitter().emitEntity(def);
    ASSERT_CONTAINS(content, "public UnityEngine.Vector3 Position { get; set; }");
    ASSERT_CONTAINS(content, "writer.Write(Position.x); writer.Write(Position.y); writer.Write(Position.z);");
    ASSERT_CONTAINS(content, "new UnityEngine.Vector3(_x, _y, _z)");
    PASS();
}

// 12. custom namespace name flows through
static void test_custom_namespace() {
    TEST("test_custom_namespace");
    CSharpEmitOptions opts;
    opts.namespaceName = "MyGame.Net";
    CSharpEmitter emitter(opts);
    EntityDef def("Avatar");
    def.addProperty("hp", PropertyType::Float32);
    auto [_, content] = emitter.emitEntity(def);
    ASSERT_CONTAINS(content, "namespace MyGame.Net");
    PASS();
}

// 13. methods without args produce empty arg list
static void test_emit_method_no_args() {
    TEST("test_emit_method_no_args");
    EntityDef def("Avatar");
    def.addMethod("ping", MethodSide::Base, {});
    auto [_, content] = CSharpEmitter().emitEntity(def);
    ASSERT_CONTAINS(content, "public void Call_Ping()");
    ASSERT_CONTAINS(content, "InvokeServerMethod(0);");
    ASSERT_NOT_CONTAINS(content, "InvokeServerMethod(0,);");
    PASS();
}

int main() {
    test_csharp_type_mapping();
    test_emit_simple_entity();
    test_emit_client_sync_property();
    test_emit_exposed_methods();
    test_emit_client_callback_method();
    test_emit_method_with_args();
    test_emit_registry();
    test_emit_aggregate();
    test_emit_no_header();
    test_pascal_case_conversion();
    test_vector3_serialization();
    test_custom_namespace();
    test_emit_method_no_args();

    std::cout << "  passed=" << testsPassed << " failed=" << testsFailed << "\n";
    return testsFailed == 0 ? 0 : 1;
}
