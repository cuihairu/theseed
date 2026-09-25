#include "theseed/core/EntityDefRegistry.h"
#include "theseed/runtime/Entity.h"
#include "theseed/runtime/EntityDef.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

using theseed::core::EntityDefRegistry;
using theseed::runtime::Entity;
using theseed::runtime::EntityDef;
using theseed::runtime::EntitySide;
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

static bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << content;
    return true;
}

// 分支覆盖：loadDirectory 的目录项过滤（子目录/未知扩展名/坏 XML/.def 放行）
// 与继承解析的自引用防御臂。
static void testDirectoryAndResolveEdges() {
    TEST("registry: directory item filters and resolve defensive arms");

    const std::string dir = "test_registry_dir_edges";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directory(dir);
    std::filesystem::create_directory(dir + "/nested");   // 非普通文件：跳过

    writeFile(dir + "/readme.txt", "not a def");          // 未知扩展名：xml 短路臂拒绝
    writeFile(dir + "/Plain.def", R"(
<EntityDef name="PlainDef">
    <Properties><Property name="p" type="Int32"/></Properties>
</EntityDef>
)");                                                     // .def 扩展名放行
    writeFile(dir + "/Broken.xml", "<EntityDef name=\"Broken\"");  // 解析失败：不计数
    writeFile(dir + "/Narcissus.xml", R"(
<EntityDef name="Narcissus" extends="Narcissus">
    <Properties><Property name="n" type="Int32"/></Properties>
</EntityDef>
)");                                                     // 自引用 extends：防御臂

    EntityDefRegistry registry;
    auto count = registry.loadDirectory(dir);

    bool ok = count == 2;  // Plain.def + Narcissus.xml
    ok = ok && registry.hasDef("PlainDef");
    ok = ok && registry.hasDef("Narcissus");
    ok = ok && registry.getDef("Narcissus")->parentType() == "Narcissus";
    ok = ok && registry.getDef("Narcissus")->propertyCount() == 1;  // 自引用未触发合并

    std::filesystem::remove_all(dir);
    if (ok) PASS();
    else FAIL("directory edges mismatch, count=" + std::to_string(count));
}

static void testRegisterDef() {
    TEST("register and retrieve definition");

    EntityDefRegistry registry;
    auto def = std::make_shared<EntityDef>("Avatar");
    def->addProperty("level", PropertyType::Int32);

    bool ok = registry.registerDef(def);
    ok = ok && registry.hasDef("Avatar");
    ok = ok && registry.getDef("Avatar") != nullptr;
    ok = ok && registry.getDef("Avatar")->entityType() == "Avatar";
    ok = ok && registry.defCount() == 1;

    if (ok) PASS(); else FAIL("register def failed");
}

static void testLoadFile() {
    TEST("load definition from XML file");

    const char* path = "test_registry_entity.xml";
    writeFile(path, R"(
<EntityDef name="Monster">
    <Properties>
        <Property name="hp" type="Int32"/>
        <Property name="speed" type="Float32"/>
    </Properties>
</EntityDef>
)");

    EntityDefRegistry registry;
    bool ok = registry.loadFile(path);
    ok = ok && registry.hasDef("Monster");
    ok = ok && registry.getDef("Monster")->propertyCount() == 2;

    std::filesystem::remove(path);

    if (ok) PASS(); else FAIL("load file failed");
}

static void testLoadDirectory() {
    TEST("load definitions from directory");

    std::string dir = "test_registry_dir";
    std::filesystem::create_directory(dir);

    writeFile(dir + "/Avatar.xml", R"(
<EntityDef name="Avatar">
    <Properties>
        <Property name="level" type="Int32"/>
    </Properties>
</EntityDef>
)");

    writeFile(dir + "/Monster.xml", R"(
<EntityDef name="Monster">
    <Properties>
        <Property name="hp" type="Int32"/>
    </Properties>
</EntityDef>
)");

    writeFile(dir + "/readme.txt", "not a def file");

    EntityDefRegistry registry;
    auto count = registry.loadDirectory(dir);

    bool ok = count == 2;
    ok = ok && registry.hasDef("Avatar");
    ok = ok && registry.hasDef("Monster");
    ok = ok && registry.defCount() == 2;

    std::filesystem::remove_all(dir);

    if (ok) PASS(); else FAIL("load directory failed, count=" + std::to_string(count));
}

static void testCreateFactory() {
    TEST("create factory and build entity");

    EntityDefRegistry registry;
    auto def = std::make_shared<EntityDef>("Avatar");
    def->addProperty("level", PropertyType::Int32);
    def->addProperty("hp", PropertyType::Float32);
    registry.registerDef(def);

    auto factory = registry.createFactory("Avatar");
    bool ok = static_cast<bool>(factory);

    auto entity = factory(1, EntitySide::Base);
    ok = ok && entity != nullptr;
    ok = ok && entity->entityType() == "Avatar";
    ok = ok && entity->side() == EntitySide::Base;
    ok = ok && entity->id() == 1;

    entity->setProperty<std::int32_t>(0, 42);
    ok = ok && entity->getProperty<std::int32_t>(0) == 42;

    if (ok) PASS(); else FAIL("create factory failed");
}

static void testEntityTypes() {
    TEST("list entity types");

    EntityDefRegistry registry;
    registry.registerDef(std::make_shared<EntityDef>("Avatar"));
    registry.registerDef(std::make_shared<EntityDef>("Monster"));
    registry.registerDef(std::make_shared<EntityDef>("Npc"));

    auto types = registry.entityTypes();
    bool ok = types.size() == 3;

    std::sort(types.begin(), types.end());
    ok = ok && types[0] == "Avatar";
    ok = ok && types[1] == "Monster";
    ok = ok && types[2] == "Npc";

    if (ok) PASS(); else FAIL("entity types list failed");
}

static void testDuplicateRegister() {
    TEST("duplicate register overwrites");

    EntityDefRegistry registry;

    auto def1 = std::make_shared<EntityDef>("Avatar");
    def1->addProperty("level", PropertyType::Int32);
    registry.registerDef(def1);

    auto def2 = std::make_shared<EntityDef>("Avatar");
    def2->addProperty("level", PropertyType::Int32);
    def2->addProperty("name", PropertyType::String);
    registry.registerDef(def2);

    bool ok = registry.defCount() == 1;
    ok = ok && registry.getDef("Avatar")->propertyCount() == 2;

    if (ok) PASS(); else FAIL("duplicate register failed");
}

static void testGetNonexistent() {
    TEST("get nonexistent returns null");

    EntityDefRegistry registry;
    bool ok = !registry.hasDef("NotExist");
    ok = ok && registry.getDef("NotExist") == nullptr;
    ok = ok && registry.createFactory("NotExist") == nullptr;

    if (ok) PASS(); else FAIL("nonexistent check failed");
}

// --- Inheritance tests ---

static void testInheritanceFromFiles() {
    TEST("inheritance: child gets parent properties and methods");

    std::string dir = "test_registry_inherit";
    std::filesystem::create_directory(dir);

    writeFile(dir + "/Base.xml", R"(
<EntityDef name="Base">
    <Properties>
        <Property name="hp" type="Int32" defaultValue="100"/>
        <Property name="level" type="Int32"/>
    </Properties>
    <Methods>
        <Method name="onDamage" side="Cell"/>
    </Methods>
</EntityDef>
)");

    writeFile(dir + "/Child.xml", R"(
<EntityDef name="Child" extends="Base">
    <Properties>
        <Property name="mana" type="Int32"/>
    </Properties>
    <Methods>
        <Method name="onCast" side="Base"/>
    </Methods>
</EntityDef>
)");

    EntityDefRegistry registry;
    auto count = registry.loadDirectory(dir);

    bool ok = count == 2;
    auto& child = registry.getDef("Child");
    ok = ok && child != nullptr;
    ok = ok && child->propertyCount() == 3;  // hp + level + mana
    ok = ok && child->methodCount() == 2;    // onDamage + onCast
    ok = ok && child->findProperty("hp") != nullptr;
    ok = ok && child->findProperty("mana") != nullptr;
    ok = ok && child->findMethod("onDamage") != nullptr;
    ok = ok && child->findMethod("onCast") != nullptr;

    std::filesystem::remove_all(dir);
    if (ok) PASS(); else FAIL("props=" + std::to_string(child->propertyCount())
                               + " methods=" + std::to_string(child->methodCount()));
}

static void testInheritanceMultiLevel() {
    TEST("inheritance: A -> B -> C chain resolves correctly");

    std::string dir = "test_registry_multi_inherit";
    std::filesystem::create_directory(dir);

    writeFile(dir + "/Root.xml", R"(
<EntityDef name="Root">
    <Properties><Property name="id" type="Int32"/></Properties>
</EntityDef>
)");
    writeFile(dir + "/Mid.xml", R"(
<EntityDef name="Mid" extends="Root">
    <Properties><Property name="hp" type="Int32"/></Properties>
</EntityDef>
)");
    writeFile(dir + "/Leaf.xml", R"(
<EntityDef name="Leaf" extends="Mid">
    <Properties><Property name="name" type="String"/></Properties>
</EntityDef>
)");

    EntityDefRegistry registry;
    registry.loadDirectory(dir);

    auto& leaf = registry.getDef("Leaf");
    bool ok = leaf != nullptr;
    ok = ok && leaf->propertyCount() == 3;
    ok = ok && leaf->findProperty("id") != nullptr;
    ok = ok && leaf->findProperty("hp") != nullptr;
    ok = ok && leaf->findProperty("name") != nullptr;

    // Mid should also have Root's properties
    auto& mid = registry.getDef("Mid");
    ok = ok && mid != nullptr;
    ok = ok && mid->propertyCount() == 2;

    std::filesystem::remove_all(dir);
    if (ok) PASS(); else FAIL("leaf props=" + std::to_string(leaf->propertyCount()));
}

static void testInheritedDefCreatesEntity() {
    TEST("inheritance: factory from inherited def creates valid entity");

    std::string dir = "test_registry_inherit_factory";
    std::filesystem::create_directory(dir);

    writeFile(dir + "/Base.xml", R"(
<EntityDef name="Base">
    <Properties>
        <Property name="hp" type="Int32" defaultValue="100"/>
        <Property name="speed" type="Float32"/>
    </Properties>
</EntityDef>
)");
    writeFile(dir + "/Child.xml", R"(
<EntityDef name="Child" extends="Base">
    <Properties>
        <Property name="mana" type="Int32" defaultValue="50"/>
    </Properties>
</EntityDef>
)");

    EntityDefRegistry registry;
    registry.loadDirectory(dir);

    auto factory = registry.createFactory("Child");
    bool ok = static_cast<bool>(factory);

    auto entity = factory(1, EntitySide::Cell);
    ok = ok && entity != nullptr;
    ok = ok && entity->entityType() == "Child";

    // Verify all properties accessible (hp=0, speed=0, mana=50)
    auto* hpDesc = registry.getDef("Child")->findProperty("hp");
    auto* manaDesc = registry.getDef("Child")->findProperty("mana");
    ok = ok && hpDesc != nullptr && manaDesc != nullptr;

    if (ok) {
        auto hp = entity->getProperty<std::int32_t>(hpDesc->id);
        auto mana = entity->getProperty<std::int32_t>(manaDesc->id);
        ok = ok && hp == 100;
        ok = ok && mana == 50;
    }

    std::filesystem::remove_all(dir);
    if (ok) PASS(); else FAIL("entity creation from inherited def failed");
}

static void testMissingParentHandled() {
    TEST("inheritance: missing parent does not crash");

    std::string dir = "test_registry_missing_parent";
    std::filesystem::create_directory(dir);

    writeFile(dir + "/Orphan.xml", R"(
<EntityDef name="Orphan" extends="NonExistent">
    <Properties><Property name="x" type="Int32"/></Properties>
</EntityDef>
)");

    EntityDefRegistry registry;
    auto count = registry.loadDirectory(dir);

    bool ok = count == 1;
    auto& orphan = registry.getDef("Orphan");
    ok = ok && orphan != nullptr;
    ok = ok && orphan->parentType() == "NonExistent";
    // Only own properties, no merge happened
    ok = ok && orphan->propertyCount() == 1;

    std::filesystem::remove_all(dir);
    if (ok) PASS(); else FAIL("props=" + std::to_string(orphan->propertyCount()));
}

static void testInheritanceWithFlags() {
    TEST("inheritance: property flags preserved from parent");

    std::string dir = "test_registry_inherit_flags";
    std::filesystem::create_directory(dir);

    writeFile(dir + "/Base.xml", R"(
<EntityDef name="Base">
    <Properties>
        <Property name="saveMe" type="Int32" flags="Persistent"/>
        <Property name="cellOnly" type="Float32" flags="Cell"/>
    </Properties>
</EntityDef>
)");
    writeFile(dir + "/Child.xml", R"(
<EntityDef name="Child" extends="Base">
    <Properties>
        <Property name="ownProp" type="Int32" flags="Base"/>
    </Properties>
</EntityDef>
)");

    EntityDefRegistry registry;
    registry.loadDirectory(dir);

    auto& child = registry.getDef("Child");
    bool ok = child != nullptr;

    auto* saveMe = child->findProperty("saveMe");
    auto* cellOnly = child->findProperty("cellOnly");
    auto* ownProp = child->findProperty("ownProp");

    ok = ok && saveMe != nullptr;
    ok = ok && cellOnly != nullptr;
    ok = ok && ownProp != nullptr;

    using PF = theseed::runtime::PropertyFlag;
    ok = ok && theseed::runtime::hasFlag(saveMe->flags, PF::Persistent);
    ok = ok && theseed::runtime::hasFlag(cellOnly->flags, PF::Cell);
    ok = ok && theseed::runtime::hasFlag(ownProp->flags, PF::Base);

    std::filesystem::remove_all(dir);
    if (ok) PASS(); else FAIL("flag check failed");
}

static void testMergeFromIsIdempotent() {
    TEST("inheritance: mergeFrom called twice is no-op");

    EntityDef parent("Parent");
    parent.addProperty("hp", PropertyType::Int32);

    EntityDef child("Child");
    child.setParentType("Parent");

    bool first = child.mergeFrom(parent);
    bool second = child.mergeFrom(parent);

    bool ok = first == true && second == false;
    ok = ok && child.propertyCount() == 1;

    if (ok) PASS(); else FAIL("first=" + std::to_string(first) + " second=" + std::to_string(second));
}

// --- Error paths: loadFile 失败分支与 EntityDef 直接构造的边界 ---

static void testExtendsCycle() {
    TEST("inheritance: extends cycle terminates without hanging");

    std::string dir = "test_registry_cycle";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directory(dir);

    writeFile(dir + "/A.xml", R"(
<EntityDef name="A" extends="B">
    <Properties><Property name="pa" type="Int32"/></Properties>
</EntityDef>
)");
    writeFile(dir + "/B.xml", R"(
<EntityDef name="B" extends="A">
    <Properties><Property name="pb" type="Int32"/></Properties>
</EntityDef>
)");

    EntityDefRegistry registry;
    bool ok = registry.loadDirectory(dir) == 2;

    // 环不致命：resolved 标记让递归提前收敛，两个定义均可获取
    ok = ok && registry.getDef("A") != nullptr;
    ok = ok && registry.getDef("B") != nullptr;

    std::filesystem::remove_all(dir);
    if (ok) PASS(); else FAIL("cycle broke registry");
}

static void testLoadFileErrorPaths() {
    TEST("loadFile error paths / registerDef rejects");

    EntityDefRegistry registry;
    // 不存在的文件：loader 抛异常 → registry 捕获返回 false
    bool ok = !registry.loadFile("no_such_def_file_xyz.xml");

    // 非 XML 内容：root.children 为空 → loader 抛异常 → 捕获返回 false
    writeFile("broken_def.xml", "this is not xml at all");
    ok = ok && !registry.loadFile("broken_def.xml");

    // 根标签不是 EntityDef → loader 抛 "Expected root tag" → 捕获返回 false
    writeFile("broken_def.xml", "<WrongTag name=\"x\"/>");
    ok = ok && !registry.loadFile("broken_def.xml");

    // 合法 XML 但缺 name 属性 → 空类型名 → registry 拒绝
    writeFile("broken_def.xml", "<EntityDef><Properties/></EntityDef>");
    ok = ok && !registry.loadFile("broken_def.xml");
    std::filesystem::remove("broken_def.xml");

    // fixedSizeOfType 对可变长类型直接返回 0
    ok = ok && EntityDef::fixedSizeOfType(PropertyType::String) == 0;
    ok = ok && EntityDef::fixedSizeOfType(PropertyType::Blob) == 0;

    // registerDef 拒绝空指针与空类型名
    ok = ok && !registry.registerDef(nullptr);
    ok = ok && !registry.registerDef(std::make_shared<EntityDef>(""));

    // mergeFrom：属性重名拒绝；方法重名跳过（continue）不拒绝整个合并；
    // 二次合并（已 inherited）拒绝
    auto base = std::make_shared<EntityDef>("Base2");
    base->addProperty("bhp", PropertyType::Int32);
    // 可变长类型属性（String/Blob 的尺寸描述为 0）
    base->addProperty("bname", PropertyType::String);
    base->addProperty("bdata", PropertyType::Blob);
    base->addMethod("shared", theseed::runtime::MethodSide::Cell);
    auto childDup = std::make_shared<EntityDef>("ChildDup");
    childDup->addProperty("bhp", PropertyType::Int32);
    ok = ok && !childDup->mergeFrom(*base);  // bhp 重名 → false
    auto child = std::make_shared<EntityDef>("Child2");
    child->addProperty("cmp", PropertyType::Int32);
    child->addMethod("shared", theseed::runtime::MethodSide::Cell);
    ok = ok && child->mergeFrom(*base);      // 方法重名仅跳过，合并成功
    ok = ok && !child->mergeFrom(*base);     // 二次合并 → false

    // addMethod：空名与重名抛 invalid_argument
    bool threw = false;
    try {
        child->addMethod("", theseed::runtime::MethodSide::Cell);
    } catch (const std::invalid_argument&) { threw = true; }
    ok = ok && threw;
    threw = false;
    child->addMethod("onTick", theseed::runtime::MethodSide::Cell);
    try {
        child->addMethod("onTick", theseed::runtime::MethodSide::Base);
    } catch (const std::invalid_argument&) { threw = true; }
    ok = ok && threw;

    // property/method 越界抛 out_of_range
    threw = false;
    try {
        static_cast<void>(child->property(999));
    } catch (const std::out_of_range&) { threw = true; }
    ok = ok && threw;
    threw = false;
    try {
        static_cast<void>(child->method(999));
    } catch (const std::out_of_range&) { threw = true; }
    ok = ok && threw;

    if (ok) PASS(); else FAIL("error path behavior wrong");
}

int main() {
    std::cout << "EntityDefRegistry tests:\n";

    testRegisterDef();
    testLoadFile();
    testLoadDirectory();
    testCreateFactory();
    testEntityTypes();
    testDuplicateRegister();
    testGetNonexistent();

    // Inheritance tests
    testInheritanceFromFiles();
    testInheritanceMultiLevel();
    testInheritedDefCreatesEntity();
    testMissingParentHandled();
    testExtendsCycle();
    testInheritanceWithFlags();
    testMergeFromIsIdempotent();
    testLoadFileErrorPaths();
    testDirectoryAndResolveEdges();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
