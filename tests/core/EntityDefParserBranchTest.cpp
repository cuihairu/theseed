// EntityDefLoader 手写 XML 解析器的分支覆盖测试：畸形输入矩阵与写法变体。
// 与 EntityDefLoaderTest 互补——那边是正常 XML 的功能断言，这边专收
// 解析器的边界与畸形输入分支（截断、变体引号、空白、外来子节点）。
#include "theseed/core/EntityDefLoader.h"
#include "theseed/runtime/EntityDef.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

using theseed::core::EntityDefLoader;
using theseed::runtime::EntityDef;
using theseed::runtime::PropertyType;

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name)                                           \
    do {                                                     \
        std::cout << "  " << (name) << "... " << std::flush; \
    } while (0)

#define PASS()                    \
    do {                          \
        std::cout << "OK\n";      \
        ++testsPassed;            \
    } while (0)

#define FAIL(msg)                               \
    do {                                        \
        std::cout << "FAILED: " << (msg) << "\n"; \
        ++testsFailed;                          \
    } while (0)

namespace {

// 畸形输入应抛 runtime_error 而非崩溃；返回是否确实抛了。
bool expectThrows(const std::string& xml) {
    try {
        EntityDefLoader::loadFromString(xml);
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

// 把 float 默认值读回来比较。
float readFloat(const std::vector<std::byte>& bytes, std::size_t offset) {
    float v = 0;
    std::memcpy(&v, bytes.data() + offset, sizeof(float));
    return v;
}

}  // namespace

static void testWhitespaceAndQuoteVariants() {
    TEST("CRLF / tab / single-quote / spaced-equals variants");

    // \r\n 行尾、\t 缩进、单引号属性值、等号两侧空白、属性区跨行。
    const char* xml =
        "<EntityDef name='Tabbed'>\r\n"
        "\t<Properties>\r\n"
        "\t\t<Property name = \"a\"\r\n"
        "\t\t        type = 'Int32'/>\r\n"
        "\t</Properties>\r\n"
        "</EntityDef>\r\n";

    auto def = EntityDefLoader::loadFromString(xml);
    bool ok = def != nullptr;
    ok = ok && def->entityType() == "Tabbed";
    ok = ok && def->propertyCount() == 1;
    ok = ok && def->findProperty("a") != nullptr;
    ok = ok && def->findProperty("a")->type == PropertyType::Int32;

    if (ok) PASS(); else FAIL("whitespace variants load failed");
}

static void testAttrValueEdgeCases() {
    TEST("attribute value edge cases (empty value / unclosed quote / empty key)");

    // 空属性值：EntityDef name="" 合法（实体名为空串）。
    {
        auto def = EntityDefLoader::loadFromString("<EntityDef name=\"\"><Properties/></EntityDef>");
        bool ok = def->entityType().empty();
        if (ok) PASS(); else FAIL("empty attr value failed");
    }

    // 未闭合引号：值吞到文档尾，解析不出根节点。
    {
        bool threw = expectThrows("<EntityDef name=\"Unclosed><Properties>");
        if (threw) PASS(); else FAIL("unclosed quote accepted");
    }

    // 空 key：属性被丢弃，实体无名但解析成功。
    {
        auto def = EntityDefLoader::loadFromString("<EntityDef =\"x\"/>");
        bool ok = def != nullptr && def->entityType().empty();
        if (ok) PASS(); else FAIL("empty attr key failed");
    }

    // 属性区裸 token（无 =）：attrs 循环在 '=' 判定处 break，裸 token 被静默
    // 丢弃，'>' 仍正常收尾——解析器容错接受。
    {
        auto def = EntityDefLoader::loadFromString("<EntityDef name=\"A\" stray><Properties/></EntityDef>");
        bool ok = def != nullptr && def->entityType() == "A";
        if (ok) PASS(); else FAIL("bare token rejected");
    }
}

static void testTruncatedDocuments() {
    TEST("truncated documents all throw");

    struct Case {
        const char* xml;
        const char* what;
    };
    const Case cases[] = {
        {"</EntityDef", "closing tag without '>'"},
        {"<?xml version=\"1.0\"", "unclosed xml declaration"},
        {"<!-- just a comment", "unclosed comment"},
        {"<", "lone '<'"},
        {"<EntityDef name=\"A", "attrs region EOF"},
        {"<EntityDef name=\"A" "   ", "trailing blanks to EOF"},
        {"<EntityDef name  ", "key terminated by blanks at EOF"},
        {"<EntityDef name", "unterminated key at EOF"},
        {"<EntityDef name=", "equals then EOF at value region"},
        {"<EntityDef", "unterminated tag at EOF"},
        {"<Entity\nDef name=\"NL\"/>", "newline terminates tag"},
        {"<Entity\tDef name=\"Tab\"/>", "tab terminates tag"},
        {"   \n\t  ", "whitespace-only input"},
        {"", "empty input"},
    };

    bool all = true;
    for (const auto& c : cases) {
        if (!expectThrows(c.xml)) {
            all = false;
            std::cout << "\n    accepted: [" << c.what << "]";
        }
    }

    if (all) PASS(); else FAIL("truncated document accepted");
}

static void testMalformedTagStructures() {
    TEST("malformed tag structures");

    // '/' 后 EOF：自闭合判定失败但节点已按自闭合收下，解析成功。
    {
        auto def = EntityDefLoader::loadFromString("<EntityDef name=\"Dangling\" /");
        bool ok = def != nullptr && def->entityType() == "Dangling";
        if (ok) PASS(); else FAIL("dangling '/' rejected");
    }

    // '/' 与 '>' 之间带空白：仍是自闭合，合法。
    {
        auto def = EntityDefLoader::loadFromString("<EntityDef name=\"Spaced\" / >");
        bool ok = def->entityType() == "Spaced";
        if (ok) PASS(); else FAIL("spaced self-closing failed");
    }

    // '/' 后跟垃圾：自闭合判定失败，垃圾当正文丢弃，已解析节点保留。
    {
        auto def = EntityDefLoader::loadFromString(
            "<EntityDef name=\"Junk\">"
            "<Properties><Property name=\"a\" type=\"Int32\" / junk></EntityDef>");
        bool ok = def != nullptr;
        ok = ok && def->propertyCount() == 1;
        if (ok) PASS(); else FAIL("'/ junk' variant failed");
    }

    // key 以 \t / \n 终止（终止集含制表与换行）：skipWs 后正常接 '='。
    {
        auto def = EntityDefLoader::loadFromString("<EntityDef name\t=\"x\"/>");
        bool ok1 = def != nullptr && def->entityType() == "x";
        auto def2 = EntityDefLoader::loadFromString("<EntityDef name\n= \"y\"/>");
        ok1 = ok1 && def2 != nullptr && def2->entityType() == "y";
        if (ok1) PASS(); else FAIL("key terminated by tab/newline failed");
    }

    // 连续 '='：第二个等号进入无引号值区，值吞到 EOF，文档收不了尾——抛错。
    {
        bool threw = expectThrows("<EntityDef name==/>");
        if (threw) PASS(); else FAIL("double equals accepted");
    }

    // 无引号属性值：值累积吞到下一个引号才停——解析器怪癖，照单断言。
    // （值里没有引号时会吞到文档尾导致解析失败，见上组"未闭合引号"。）
    {
        auto def = EntityDefLoader::loadFromString("<EntityDef name=x y=\"2\"/>");
        bool ok = def != nullptr && def->entityType() == "x y=";
        if (ok) PASS(); else FAIL("unquoted attr value failed");
    }

    // 引号也是合法 key 字符：带引号的裸 token 同样被静默丢弃。
    {
        auto def = EntityDefLoader::loadFromString("<EntityDef name=\"Q\" \"stray\"><Properties/></EntityDef>");
        bool ok = def != nullptr && def->entityType() == "Q";
        if (ok) PASS(); else FAIL("quoted stray token rejected");
    }
}

static void testForeignChildNodes() {
    TEST("foreign child nodes are skipped");

    // Properties 下的非 Property 子节点被 continue 跳过。
    {
        auto def = EntityDefLoader::loadFromString(
            "<EntityDef name=\"Foreign\">"
            "<Properties>"
            "<Bogus attr=\"x\"/>"
            "<Property name=\"a\" type=\"Int32\"/>"
            "</Properties>"
            "</EntityDef>");
        bool ok = def->propertyCount() == 1;
        ok = ok && def->findProperty("a") != nullptr;
        if (ok) PASS(); else FAIL("foreign Property child rejected");
    }

    // EntityDef 下的既非 Properties 也非 Methods 的块被跳过。
    {
        auto def = EntityDefLoader::loadFromString(
            "<EntityDef name=\"Extra\">"
            "<Properties><Property name=\"a\" type=\"Int32\"/></Properties>"
            "<Scripts/>"
            "</EntityDef>");
        bool ok = def->propertyCount() == 1 && def->methodCount() == 0;
        if (ok) PASS(); else FAIL("unknown entity child rejected");
    }

    // 空 Methods 块 + Methods 下的非 Method 子节点。
    {
        auto def = EntityDefLoader::loadFromString(
            "<EntityDef name=\"Meth\">"
            "<Methods/>"
            "</EntityDef>");
        bool ok = def->methodCount() == 0;

        auto def2 = EntityDefLoader::loadFromString(
            "<EntityDef name=\"Meth2\">"
            "<Methods>"
            "<Note/>"
            "<Method name=\"ping\" side=\"Base\">"
            "<Arg name=\"v\" type=\"Int32\"/>"
            "<Bogus/>"
            "</Method>"
            "</Methods>"
            "</EntityDef>");
        ok = ok && def2->methodCount() == 1;
        auto* m = def2->findMethod("ping");
        ok = ok && m != nullptr && m->args.size() == 1;

        if (ok) PASS(); else FAIL("methods foreign children failed");
    }
}

static void testDefaultMatrixRemainingTypes() {
    TEST("defaultValue matrix for remaining fixed-size types");

    // testNumericDefaultValues 已收 Int8/Int16/Int64/Float64/Bool；
    // 这里补 UInt 系、Int32/UInt32、Float32、UInt64，凑齐 switch 全 case。
    const char* xml = R"(
<EntityDef name="UnsignedDefaults">
    <Properties>
        <Property name="u8"  type="UInt8"  defaultValue="200"/>
        <Property name="u16" type="UInt16" defaultValue="60000"/>
        <Property name="i32" type="Int32"  defaultValue="-70000"/>
        <Property name="u32" type="UInt32" defaultValue="4294967295"/>
        <Property name="u64" type="UInt64" defaultValue="18446744073709551615"/>
        <Property name="f32" type="Float32" defaultValue="1.5"/>
    </Properties>
</EntityDef>
)";

    auto def = EntityDefLoader::loadFromString(xml);
    bool ok = def != nullptr;

    const auto* u8 = def->findProperty("u8");
    ok = ok && u8->defaultValue.size() == 1;
    ok = ok && u8->defaultValue[0] == std::byte{200};

    const auto* u16 = def->findProperty("u16");
    ok = ok && u16->defaultValue.size() == 2;
    std::uint16_t u16v = 0;
    std::memcpy(&u16v, u16->defaultValue.data(), 2);
    ok = ok && u16v == 60000;

    const auto* i32 = def->findProperty("i32");
    ok = ok && i32->defaultValue.size() == 4;
    std::int32_t i32v = 0;
    std::memcpy(&i32v, i32->defaultValue.data(), 4);
    ok = ok && i32v == -70000;

    const auto* u32 = def->findProperty("u32");
    ok = ok && u32->defaultValue.size() == 4;
    std::uint32_t u32v = 0;
    std::memcpy(&u32v, u32->defaultValue.data(), 4);
    // UINT32_MAX 超出 stoi 上限：无符号解析（stoull）必须收下全部合法域值。
    ok = ok && u32v == 4294967295u;

    const auto* u64 = def->findProperty("u64");
    ok = ok && u64->defaultValue.size() == 8;
    std::uint64_t u64v = 0;
    std::memcpy(&u64v, u64->defaultValue.data(), 8);
    // UINT64_MAX 超出 stoll 上限：无符号解析（stoull）必须收下全部合法域值。
    ok = ok && u64v == 18446744073709551615ull;

    const auto* f32 = def->findProperty("f32");
    ok = ok && f32->defaultValue.size() == 4;
    ok = ok && readFloat(f32->defaultValue, 0) == 1.5f;

    if (ok) PASS(); else FAIL("unsigned/float32 default matrix failed");
}

static void testBlobOddLengthDefault() {
    TEST("blob default with odd hex length yields empty default");

    const char* xml = R"(
<EntityDef name="OddBlob">
    <Properties>
        <Property name="payload" type="Blob" defaultValue="DEADBEE"/>
    </Properties>
</EntityDef>
)";

    auto def = EntityDefLoader::loadFromString(xml);
    bool ok = def != nullptr;
    const auto* prop = def->findProperty("payload");
    ok = ok && prop != nullptr && prop->type == PropertyType::Blob;
    ok = ok && prop->defaultValue.empty();

    if (ok) PASS(); else FAIL("odd-length blob default failed");
}

static void testVector3TokenVariants() {
    TEST("Vector3 default token variants");

    const char* xml = R"(
<EntityDef name="VecVariants">
    <Properties>
        <Property name="four" type="Vector3" defaultValue="1,2,3,4"/>
        <Property name="trail" type="Vector3" defaultValue="5,6,"/>
        <Property name="two" type="Vector3" defaultValue="7,8"/>
        <Property name="five" type="Vector3" defaultValue="1,2,3,4,"/>
        <Property name="lead" type="Vector3" defaultValue=",1,2,3"/>
    </Properties>
</EntityDef>
)";

    auto def = EntityDefLoader::loadFromString(xml);
    bool ok = def != nullptr;

    // 第四个 token 丢弃，保留前三段。
    const auto* four = def->findProperty("four");
    ok = ok && four->defaultValue.size() == 12;
    ok = ok && readFloat(four->defaultValue, 0) == 1.f;
    ok = ok && readFloat(four->defaultValue, 4) == 2.f;
    ok = ok && readFloat(four->defaultValue, 8) == 3.f;

    // 尾逗号：空尾 token 不消费下标，第三段留 0。
    const auto* trail = def->findProperty("trail");
    ok = ok && trail->defaultValue.size() == 12;
    ok = ok && readFloat(trail->defaultValue, 0) == 5.f;
    ok = ok && readFloat(trail->defaultValue, 4) == 6.f;
    ok = ok && readFloat(trail->defaultValue, 8) == 0.f;

    // 两段：最终 token 在收尾分支赋值。
    const auto* two = def->findProperty("two");
    ok = ok && two->defaultValue.size() == 12;
    ok = ok && readFloat(two->defaultValue, 0) == 7.f;
    ok = ok && readFloat(two->defaultValue, 4) == 8.f;
    ok = ok && readFloat(two->defaultValue, 8) == 0.f;

    // 尾随第 4 个逗号：idx 已满 3，逗号分支短路，第 5 段被丢弃。
    const auto* five = def->findProperty("five");
    ok = ok && readFloat(five->defaultValue, 0) == 1.f;
    ok = ok && readFloat(five->defaultValue, 4) == 2.f;
    ok = ok && readFloat(five->defaultValue, 8) == 3.f;

    // 前导逗号：空首 token 被跳过，不下标推进。
    const auto* lead = def->findProperty("lead");
    ok = ok && readFloat(lead->defaultValue, 0) == 1.f;
    ok = ok && readFloat(lead->defaultValue, 4) == 2.f;
    ok = ok && readFloat(lead->defaultValue, 8) == 3.f;

    if (ok) PASS(); else FAIL("Vector3 token variants failed");
}

static void testMinMaxOnVariableSized() {
    TEST("min/max on variable-sized types is ignored");

    const char* xml = R"(
<EntityDef name="StrMinMax">
    <Properties>
        <Property name="nick" type="String" minValue="1" maxValue="32"/>
        <Property name="data" type="Blob" minValue="AA" maxValue="BB"/>
    </Properties>
</EntityDef>
)";

    auto def = EntityDefLoader::loadFromString(xml);
    bool ok = def != nullptr;

    const auto* nick = def->findProperty("nick");
    ok = ok && nick != nullptr && nick->type == PropertyType::String;
    ok = ok && nick->minValue.empty() && nick->maxValue.empty();

    const auto* data = def->findProperty("data");
    ok = ok && data != nullptr && data->type == PropertyType::Blob;
    ok = ok && data->minValue.empty() && data->maxValue.empty();

    if (ok) PASS(); else FAIL("variable-sized min/max failed");
}

int main() {
    std::cout << "EntityDef parser branch tests:\n";

    testWhitespaceAndQuoteVariants();
    testAttrValueEdgeCases();
    testTruncatedDocuments();
    testMalformedTagStructures();
    testForeignChildNodes();
    testDefaultMatrixRemainingTypes();
    testBlobOddLengthDefault();
    testVector3TokenVariants();
    testMinMaxOnVariableSized();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
