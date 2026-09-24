// EntityQuery 分支覆盖测试（第三批）：decodeNative 尺寸错配矩阵、浮点
// <=> 全结果矩阵（含 NaN unordered）、Bool/String 结果边、of(bool,false)。
// 与 EntityQueryTest 互补——那边是查询功能断言，这边专收 compareSameType
// 的类型矩阵边界与比较结果选择边。
#include "theseed/core/EntityQuery.h"

#include <cmath>
#include <compare>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using theseed::core::compareProperty;
using theseed::core::DataType;
using theseed::core::EntityData;
using theseed::core::PropertyData;
using theseed::core::QueryFilter;
using theseed::core::QueryOp;

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name)                                           \
    do {                                                     \
        std::cout << "  " << (name) << "... " << std::flush; \
    } while (0)

#define PASS()               \
    do {                     \
        std::cout << "OK\n"; \
        ++testsPassed;       \
    } while (0)

#define FAIL(msg)                                 \
    do {                                          \
        std::cout << "FAILED: " << (msg) << "\n"; \
        ++testsFailed;                            \
    } while (0)

namespace {

template <typename T>
std::vector<std::byte> nativeBytes(T v) {
    std::vector<std::byte> buf(sizeof(T));
    std::memcpy(buf.data(), &v, sizeof(T));
    return buf;
}

PropertyData makeProp(const std::string& name, DataType type, std::vector<std::byte> raw) {
    PropertyData p;
    p.name = name;
    p.type = type;
    p.rawValue = std::move(raw);
    return p;
}

// 字符串的 rawValue：长度前缀不编码，直接字节序列（bytesToString 原样读回）。
PropertyData makeStringProp(const std::string& name, const std::string& v) {
    std::vector<std::byte> raw;
    raw.reserve(v.size());
    for (char c : v) raw.push_back(static_cast<std::byte>(c));
    return makeProp(name, DataType::String, std::move(raw));
}

}  // namespace

static void testDecodeSizeMismatchMatrix() {
    TEST("decodeNative size mismatch yields unordered for every numeric type");

    // lhs 尺寸错（空 rawValue）×11 个数值类型 → 各实例的 decodeNative false 臂。
    struct Case {
        DataType type;
        std::vector<std::byte> good;
    };
    const Case cases[] = {
        {DataType::Int8,    nativeBytes(std::int8_t{7})},
        {DataType::Int16,   nativeBytes(std::int16_t{700})},
        {DataType::Int32,   nativeBytes(std::int32_t{70000})},
        {DataType::Int64,   nativeBytes(std::int64_t{-5'000'000'000LL})},
        {DataType::UInt8,   nativeBytes(std::uint8_t{200})},
        {DataType::UInt16,  nativeBytes(std::uint16_t{60000})},
        {DataType::UInt32,  nativeBytes(std::uint32_t{3'000'000'000u})},
        {DataType::UInt64,  nativeBytes(std::uint64_t{9'000'000'000'000'000'000ull})},
        {DataType::Float32, nativeBytes(1.5f)},
        {DataType::Float64, nativeBytes(2.5)},
        {DataType::Bool,    nativeBytes(std::uint8_t{1})},
    };

    bool ok = true;
    for (const auto& c : cases) {
        const auto lhs = makeProp("p", c.type, {});  // 空尺寸 → decode 失败
        const auto rhs = makeProp("p", c.type, c.good);
        ok = ok && compareProperty(lhs, rhs) == std::partial_ordering::unordered;
    }
    // rhs 尺寸错（filter 侧携带坏 rawValue）→ 第二个 decodeNative false 臂。
    // 逐类型截掉一字节，保证对任何定长类型都是错尺寸。
    for (const auto& c : cases) {
        const auto lhs = makeProp("p", c.type, c.good);
        const auto bad = std::vector<std::byte>(c.good.begin(), c.good.end() - 1);
        const auto rhs = makeProp("p", c.type, bad);
        ok = ok && compareProperty(lhs, rhs) == std::partial_ordering::unordered;
    }

    if (ok) PASS(); else FAIL("decode size mismatch matrix failed");
}

static void testFloatOrderingMatrix() {
    TEST("float <=> hits less/greater/equal/unordered for Float32 and Float64");

    bool ok = true;
    // Float32：<=> 的四种结果选择边。
    const auto f1 = makeProp("f", DataType::Float32, nativeBytes(1.0f));
    const auto f2 = makeProp("f", DataType::Float32, nativeBytes(2.0f));
    ok = ok && compareProperty(f1, f2) == std::partial_ordering::less;
    ok = ok && compareProperty(f2, f1) == std::partial_ordering::greater;
    ok = ok && compareProperty(f1, f1) == std::partial_ordering::equivalent;
    const auto nanF = makeProp("f", DataType::Float32, nativeBytes(std::nanf("")));
    ok = ok && compareProperty(nanF, f1) == std::partial_ordering::unordered;
    ok = ok && compareProperty(f1, nanF) == std::partial_ordering::unordered;

    // Float64：同上。
    const auto d1 = makeProp("d", DataType::Float64, nativeBytes(1.0));
    const auto d2 = makeProp("d", DataType::Float64, nativeBytes(2.0));
    ok = ok && compareProperty(d1, d2) == std::partial_ordering::less;
    ok = ok && compareProperty(d2, d1) == std::partial_ordering::greater;
    ok = ok && compareProperty(d1, d1) == std::partial_ordering::equivalent;
    const auto nanD = makeProp("d", DataType::Float64, nativeBytes(std::nan("")));
    ok = ok && compareProperty(nanD, d1) == std::partial_ordering::unordered;
    ok = ok && compareProperty(d1, nanD) == std::partial_ordering::unordered;

    if (ok) PASS(); else FAIL("float ordering matrix failed");
}

static void testBoolAndStringOrdering() {
    TEST("bool and string <=> result edges");

    bool ok = true;
    // Bool：(a!=0) <=> (b!=0) 的三个结果边。
    const auto bTrue = makeProp("b", DataType::Bool, nativeBytes(std::uint8_t{1}));
    const auto bFalse = makeProp("b", DataType::Bool, nativeBytes(std::uint8_t{0}));
    ok = ok && compareProperty(bTrue, bFalse) == std::partial_ordering::greater;
    ok = ok && compareProperty(bFalse, bTrue) == std::partial_ordering::less;
    ok = ok && compareProperty(bTrue, bTrue) == std::partial_ordering::equivalent;

    // String：bytesToString 后的三个结果边。
    const auto sA = makeStringProp("s", "alpha");
    const auto sB = makeStringProp("s", "beta");
    ok = ok && compareProperty(sA, sB) == std::partial_ordering::less;
    ok = ok && compareProperty(sB, sA) == std::partial_ordering::greater;
    ok = ok && compareProperty(sA, makeStringProp("s", "alpha")) == std::partial_ordering::equivalent;

    if (ok) PASS(); else FAIL("bool/string ordering failed");
}

static void testBoolFalseFilter() {
    TEST("QueryFilter::of with false builds zero byte");

    // of(bool) 的 v=false 臂：false 编码为 0 字节，与 true 数据不相等。
    const auto fFalse = QueryFilter::of("alive", QueryOp::Eq, false);
    const auto fTrue = QueryFilter::of("alive", QueryOp::Eq, true);

    bool ok = fFalse.rawValue.size() == 1 && fFalse.rawValue[0] == std::byte{0};
    ok = ok && fTrue.rawValue.size() == 1 && fTrue.rawValue[0] == std::byte{1};

    EntityData e;
    auto& prop = e.properties.emplace_back();
    prop.name = "alive";
    prop.type = DataType::Bool;
    prop.rawValue = nativeBytes(std::uint8_t{1});
    ok = ok && theseed::core::matchesFilter(e, fTrue);
    ok = ok && !theseed::core::matchesFilter(e, fFalse);

    if (ok) PASS(); else FAIL("bool false filter failed");
}

static void testNaNFilterNoMatch() {
    TEST("matchesFilter treats NaN as non-matching");

    EntityData e;
    auto& prop = e.properties.emplace_back();
    prop.name = "f";
    prop.type = DataType::Float32;
    prop.rawValue = nativeBytes(std::nanf(""));
    const auto filter = QueryFilter::of("f", QueryOp::Lt, 1.0f);
    const auto eqFilter = QueryFilter::of("f", QueryOp::Eq, 1.0f);
    const bool ok = !theseed::core::matchesFilter(e, filter)
                 && !theseed::core::matchesFilter(e, eqFilter);

    if (ok) PASS(); else FAIL("NaN filter match failed");
}

static void testTypeMismatch() {
    TEST("type mismatch is unordered");

    const auto i32 = makeProp("p", DataType::Int32, nativeBytes(std::int32_t{5}));
    const auto s = makeStringProp("p", "ab");
    const bool ok = compareProperty(i32, s) == std::partial_ordering::unordered;

    if (ok) PASS(); else FAIL("type mismatch failed");
}

int main() {
    std::cout << "EntityQuery branch tests:\n";

    testDecodeSizeMismatchMatrix();
    testFloatOrderingMatrix();
    testBoolAndStringOrdering();
    testBoolFalseFilter();
    testNaNFilterNoMatch();
    testTypeMismatch();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
