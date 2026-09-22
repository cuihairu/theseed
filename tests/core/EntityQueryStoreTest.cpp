#include "theseed/core/EntityData.h"
#include "theseed/core/EntityQuery.h"
#include "theseed/core/IEntityStore.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using theseed::core::DataType;
using theseed::core::EntityData;
using theseed::core::EntityId;
using theseed::core::IEntityQueryStore;
using theseed::core::IEntityStore;
using theseed::core::InMemoryEntityStore;
using theseed::core::InMemoryQueryStore;
using theseed::core::PropertyData;
using theseed::core::QueryFilter;
using theseed::core::QueryOp;
using theseed::core::StorageQuery;

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

static PropertyData makeInt32Prop(std::uint32_t id, const std::string& name, std::int32_t v) {
    PropertyData p;
    p.id = id;
    p.name = name;
    p.type = DataType::Int32;
    p.rawValue.resize(sizeof(std::int32_t));
    std::memcpy(p.rawValue.data(), &v, sizeof(std::int32_t));
    return p;
}

static PropertyData makeUInt64Prop(std::uint32_t id, const std::string& name, std::uint64_t v) {
    PropertyData p;
    p.id = id;
    p.name = name;
    p.type = DataType::UInt64;
    p.rawValue.resize(sizeof(std::uint64_t));
    std::memcpy(p.rawValue.data(), &v, sizeof(std::uint64_t));
    return p;
}

static PropertyData makeFloat32Prop(std::uint32_t id, const std::string& name, float v) {
    PropertyData p;
    p.id = id;
    p.name = name;
    p.type = DataType::Float32;
    p.rawValue.resize(sizeof(float));
    std::memcpy(p.rawValue.data(), &v, sizeof(float));
    return p;
}

static PropertyData makeStringProp(std::uint32_t id, const std::string& name, const std::string& v) {
    PropertyData p;
    p.id = id;
    p.name = name;
    p.type = DataType::String;
    p.rawValue.assign(
        reinterpret_cast<const std::byte*>(v.data()),
        reinterpret_cast<const std::byte*>(v.data()) + v.size());
    return p;
}

static PropertyData makeBoolProp(std::uint32_t id, const std::string& name, bool v) {
    PropertyData p;
    p.id = id;
    p.name = name;
    p.type = DataType::Bool;
    const std::uint8_t b = v ? 1 : 0;
    p.rawValue.resize(1);
    std::memcpy(p.rawValue.data(), &b, 1);
    return p;
}

static EntityData makeAvatar(EntityId id, std::int32_t level, std::uint64_t gold, float hp, const std::string& name, bool vip) {
    EntityData d;
    d.id = id;
    d.entityType = "Avatar";
    d.properties.push_back(makeInt32Prop(1, "level", level));
    d.properties.push_back(makeUInt64Prop(2, "gold", gold));
    d.properties.push_back(makeFloat32Prop(3, "hp", hp));
    d.properties.push_back(makeStringProp(4, "name", name));
    d.properties.push_back(makeBoolProp(5, "vip", vip));
    return d;
}

static std::shared_ptr<InMemoryEntityStore> seedStore() {
    auto store = std::make_shared<InMemoryEntityStore>();
    store->save(1, makeAvatar(1, 10, 100, 80.0f, "alice", false));
    store->save(2, makeAvatar(2, 50, 5000, 200.5f, "bob", true));
    store->save(3, makeAvatar(3, 50, 800, 150.0f, "carol", false));
    store->save(4, makeAvatar(4, 60, 12000, 250.0f, "dave", true));
    store->save(5, makeAvatar(5, 5, 50, 30.0f, "eve", false));
    return store;
}

static void test_query_int32_eq() {
    TEST("test_query_int32_eq");
    auto store = seedStore();
    InMemoryQueryStore qs(store);
    StorageQuery q;
    q.entityType = "Avatar";
    q.filters.push_back(QueryFilter::of("level", QueryOp::Eq, std::int32_t(50)));
    auto ids = qs.query(q);
    if (ids.size() != 2) { FAIL("expected 2 results"); return; }
    if (ids[0] != 2 || ids[1] != 3) { FAIL("wrong ids"); return; }
    PASS();
}

static void test_query_int32_comparisons() {
    TEST("test_query_int32_comparisons");
    auto store = seedStore();
    InMemoryQueryStore qs(store);
    StorageQuery q;
    q.entityType = "Avatar";
    q.filters.push_back(QueryFilter::of("level", QueryOp::Gt, std::int32_t(50)));
    auto ids = qs.query(q);
    if (ids.size() != 1 || ids[0] != 4) { FAIL("Gt50 expected only dave(4)"); return; }

    q.filters.clear();
    q.filters.push_back(QueryFilter::of("level", QueryOp::Ge, std::int32_t(50)));
    ids = qs.query(q);
    if (ids.size() != 3) { FAIL("Ge50 expected 3"); return; }

    q.filters.clear();
    q.filters.push_back(QueryFilter::of("level", QueryOp::Lt, std::int32_t(50)));
    ids = qs.query(q);
    if (ids.size() != 2) { FAIL("Lt50 expected 2"); return; }

    q.filters.clear();
    q.filters.push_back(QueryFilter::of("level", QueryOp::Ne, std::int32_t(50)));
    ids = qs.query(q);
    if (ids.size() != 3) { FAIL("Ne50 expected 3"); return; }
    PASS();
}

static void test_query_uint64() {
    TEST("test_query_uint64");
    auto store = seedStore();
    InMemoryQueryStore qs(store);
    StorageQuery q;
    q.entityType = "Avatar";
    q.filters.push_back(QueryFilter::of("gold", QueryOp::Ge, std::uint64_t(1000)));
    auto ids = qs.query(q);
    if (ids.size() != 2) { FAIL("expected 2 rich"); return; }
    PASS();
}

static void test_query_float32() {
    TEST("test_query_float32");
    auto store = seedStore();
    InMemoryQueryStore qs(store);
    StorageQuery q;
    q.entityType = "Avatar";
    q.filters.push_back(QueryFilter::of("hp", QueryOp::Gt, float(100.0f)));
    auto ids = qs.query(q);
    if (ids.size() != 3) { FAIL("expected 3 high hp"); return; }
    PASS();
}

static void test_query_string() {
    TEST("test_query_string");
    auto store = seedStore();
    InMemoryQueryStore qs(store);
    StorageQuery q;
    q.entityType = "Avatar";
    q.filters.push_back(QueryFilter::of("name", QueryOp::Eq, std::string("carol")));
    auto ids = qs.query(q);
    if (ids.size() != 1 || ids[0] != 3) { FAIL("expected carol=3"); return; }

    q.filters.clear();
    q.filters.push_back(QueryFilter::of("name", QueryOp::Ge, std::string("d")));
    ids = qs.query(q);
    if (ids.size() != 2) { FAIL("expected 2 names >= d"); return; }
    PASS();
}

static void test_query_bool() {
    TEST("test_query_bool");
    auto store = seedStore();
    InMemoryQueryStore qs(store);
    StorageQuery q;
    q.entityType = "Avatar";
    q.filters.push_back(QueryFilter::of("vip", QueryOp::Eq, true));
    auto ids = qs.query(q);
    if (ids.size() != 2) { FAIL("expected 2 vips"); return; }
    PASS();
}

static void test_query_and_filters() {
    TEST("test_query_and_filters");
    auto store = seedStore();
    InMemoryQueryStore qs(store);
    StorageQuery q;
    q.entityType = "Avatar";
    q.filters.push_back(QueryFilter::of("level", QueryOp::Ge, std::int32_t(50)));
    q.filters.push_back(QueryFilter::of("vip", QueryOp::Eq, true));
    auto ids = qs.query(q);
    if (ids.size() != 2) { FAIL("expected 2 vip players >= level 50"); return; }
    if (ids[0] != 2 || ids[1] != 4) { FAIL("wrong ids"); return; }
    PASS();
}

static void test_query_limit() {
    TEST("test_query_limit");
    auto store = seedStore();
    InMemoryQueryStore qs(store);
    StorageQuery q;
    q.entityType = "Avatar";
    q.limit = 2;
    auto ids = qs.query(q);
    if (ids.size() != 2) { FAIL("limit=2 should return 2"); return; }
    if (ids[0] != 1 || ids[1] != 2) { FAIL("expected sorted first 2"); return; }
    PASS();
}

static void test_query_offset() {
    TEST("test_query_offset");
    auto store = seedStore();
    InMemoryQueryStore qs(store);
    StorageQuery q;
    q.entityType = "Avatar";
    q.offset = 2;
    q.limit = 10;
    auto ids = qs.query(q);
    if (ids.size() != 3) { FAIL("offset=2 should leave 3"); return; }
    if (ids[0] != 3) { FAIL("first should be id 3"); return; }
    PASS();
}

static void test_query_missing_property() {
    TEST("test_query_missing_property");
    auto store = seedStore();
    InMemoryQueryStore qs(store);
    StorageQuery q;
    q.entityType = "Avatar";
    q.filters.push_back(QueryFilter::of("nonexistent", QueryOp::Eq, std::int32_t(42)));
    auto ids = qs.query(q);
    if (!ids.empty()) { FAIL("missing property should match nothing"); return; }
    PASS();
}

static void test_query_type_mismatch() {
    TEST("test_query_type_mismatch");
    auto store = seedStore();
    InMemoryQueryStore qs(store);
    StorageQuery q;
    q.entityType = "Avatar";
    q.filters.push_back(QueryFilter::of("level", QueryOp::Eq, std::uint32_t(50)));
    auto ids = qs.query(q);
    if (!ids.empty()) { FAIL("type mismatch should not match"); return; }
    PASS();
}

static void test_query_empty_entity_type() {
    TEST("test_query_empty_entity_type");
    auto store = seedStore();
    InMemoryQueryStore qs(store);
    StorageQuery q;
    q.entityType = "";
    auto ids = qs.query(q);
    if (!ids.empty()) { FAIL("empty type should return empty"); return; }
    PASS();
}

static void test_query_no_filter_matches_all() {
    TEST("test_query_no_filter_matches_all");
    auto store = seedStore();
    InMemoryQueryStore qs(store);
    StorageQuery q;
    q.entityType = "Avatar";
    q.limit = 100;
    auto ids = qs.query(q);
    if (ids.size() != 5) { FAIL("expected all 5 avatars"); return; }
    PASS();
}

static void test_query_unknown_type() {
    TEST("test_query_unknown_type");
    auto store = seedStore();
    InMemoryQueryStore qs(store);
    StorageQuery q;
    q.entityType = "Nonexistent";
    auto ids = qs.query(q);
    if (!ids.empty()) { FAIL("unknown type should return empty"); return; }
    PASS();
}

static void test_null_store_rejected() {
    TEST("test_null_store_rejected");
    try {
        InMemoryQueryStore qs(nullptr);
        FAIL("should have thrown");
    } catch (const std::invalid_argument&) {
        PASS();
    } catch (...) {
        FAIL("expected std::invalid_argument");
    }
}

// --- 补充：逐数值类型的比较/过滤/查询往返 ---

static std::size_t widthOf(DataType type) {
    switch (type) {
        case DataType::Int8:
        case DataType::UInt8:
            return 1;
        case DataType::Int16:
        case DataType::UInt16:
            return 2;
        case DataType::Int32:
        case DataType::UInt32:
        case DataType::Float32:
            return 4;
        default:
            return 8;
    }
}

static EntityData makeScored(EntityId id, DataType type, double value) {
    EntityData d;
    d.id = id;
    d.entityType = "Avatar";
    PropertyData p;
    p.id = 9;
    p.name = "score";
    p.type = type;
    if (type == DataType::Float32) {
        p.rawValue.resize(sizeof(float));
        const float v = static_cast<float>(value);
        std::memcpy(p.rawValue.data(), &v, sizeof(float));
    } else if (type == DataType::Float64) {
        p.rawValue.resize(sizeof(double));
        const double v = value;
        std::memcpy(p.rawValue.data(), &v, sizeof(double));
    } else {
        // decodeNative 严格按类型宽度校验，整型必须编到对应宽度。
        p.rawValue.resize(widthOf(type));
        const std::uint64_t v = static_cast<std::uint64_t>(value);
        std::memcpy(p.rawValue.data(), &v, widthOf(type));
    }
    d.properties.push_back(std::move(p));
    return d;
}

// 手工按 DataType 编码数值 filter：QueryFilter::of 的窄整数实参会经整型提升
// 落到 Int32 重载（如 int8_t→int32_t），类型与实体属性对不上会全部 unordered。
// 10/15/20 在 float/double 中可精确表示，因此一个 double 入口覆盖全部类型。
static QueryFilter numericFilter(const std::string& name, QueryOp op,
                                 DataType type, double value) {
    QueryFilter f;
    f.propertyName = name;
    f.op = op;
    f.type = type;
    if (type == DataType::Float32) {
        f.rawValue.resize(sizeof(float));
        const float v = static_cast<float>(value);
        std::memcpy(f.rawValue.data(), &v, sizeof(float));
    } else if (type == DataType::Float64) {
        f.rawValue.resize(sizeof(double));
        const double v = value;
        std::memcpy(f.rawValue.data(), &v, sizeof(double));
    } else {
        f.rawValue.resize(widthOf(type));
        const std::uint64_t v = static_cast<std::uint64_t>(value);
        std::memcpy(f.rawValue.data(), &v, widthOf(type));
    }
    return f;
}

// 对一种数值类型：六种 op 的过滤 + compareProperty 的同宽往返。
static bool checkNumericType(DataType type) {
    auto store = std::make_shared<InMemoryEntityStore>();
    store->save(1, makeScored(1, type, 10));
    store->save(2, makeScored(2, type, 20));
    InMemoryQueryStore qs(store);

    const auto queryCount = [&](QueryOp op, double v) {
        StorageQuery q;
        q.entityType = "Avatar";
        q.filters.push_back(numericFilter("score", op, type, v));
        return qs.query(q).size();
    };

    bool ok = true;
    auto expect = [&](std::size_t got, std::size_t want, const char* what) {
        if (got != want) {
            std::cout << "FAIL: " << what << " got=" << got << " want=" << want
                      << "\n";
            ++testsFailed;
            ok = false;
        }
    };

    expect(queryCount(QueryOp::Eq, 10), 1, "eq 10");
    expect(queryCount(QueryOp::Gt, 15), 1, "gt 15 (only 20)");
    expect(queryCount(QueryOp::Lt, 15), 1, "lt 15 (only 10)");
    expect(queryCount(QueryOp::Le, 10), 1, "le 10 (only 10)");
    expect(queryCount(QueryOp::Ge, 20), 1, "ge 20 (only 20)");
    expect(queryCount(QueryOp::Ne, 10), 1, "ne 10 (only 20)");
    return ok;
}

static void test_all_numeric_types_round_trip() {
    TEST("test_all_numeric_types_round_trip");
    const DataType types[] = {
        DataType::Int8,   DataType::Int16,  DataType::Int64,
        DataType::UInt8,  DataType::UInt16, DataType::UInt32,
        DataType::UInt64, DataType::Float64,
    };
    bool ok = true;
    for (auto t : types) ok = checkNumericType(t) && ok;
    if (ok) PASS();
}

static void test_filter_of_int64_and_double() {
    TEST("QueryFilter::of int64/double overloads match same-typed props");
    EntityData big;
    big.entityType = "Avatar";
    PropertyData exp;
    exp.id = 1;
    exp.name = "exp";
    exp.type = DataType::Int64;  // 与 of(std::int64_t) 生成的 filter 同类型
    exp.rawValue.resize(sizeof(std::int64_t));
    const std::int64_t expValue = 5'000'000'000LL;  // 超 int32 范围，逼出 int64 编解码
    std::memcpy(exp.rawValue.data(), &expValue, sizeof(std::int64_t));
    big.properties.push_back(exp);
    PropertyData ratio;
    ratio.id = 2;
    ratio.name = "ratio";
    ratio.type = DataType::Float64;
    ratio.rawValue.resize(sizeof(double));
    const double v = 0.75;
    std::memcpy(ratio.rawValue.data(), &v, sizeof(double));
    big.properties.push_back(ratio);

    // int64 与 int32 属性并存时靠 DataType 区分，不允许隐式混比。
    if (!theseed::core::matchesFilter(
            big, QueryFilter::of("exp", QueryOp::Ge, std::int64_t(4'999'999'999LL)))) {
        FAIL("int64 filter should match int64-width prop");
    }
    if (!theseed::core::matchesFilter(big, QueryFilter::of("ratio", QueryOp::Lt, 0.8))) {
        FAIL("double filter should match double prop");
    }
    if (theseed::core::matchesFilter(big, QueryFilter::of("ratio", QueryOp::Lt, 0.5))) {
        FAIL("double filter above value should not match");
    }
    PASS();
}

static void test_compare_property_direct() {
    TEST("test_compare_property_direct and unordered cases");
    // 同类型三态
    auto lo = makeInt32Prop(1, "v", 1);
    auto hi = makeInt32Prop(2, "v", 2);
    if (!(theseed::core::compareProperty(lo, hi) < 0)) { FAIL("lo<hi"); return; }
    if (!(theseed::core::compareProperty(hi, lo) > 0)) { FAIL("hi>lo"); return; }
    if (!(theseed::core::compareProperty(lo, lo) == 0)) { FAIL("lo==lo"); return; }

    // 类型不同 → unordered
    auto flag = makeBoolProp(3, "v", true);
    if (theseed::core::compareProperty(lo, flag) != std::partial_ordering::unordered) {
        FAIL("different types should be unordered"); return;
    }
    // Vector3 / Blob 比较无意义 → unordered
    PropertyData v3a, v3b, blobA, blobB;
    v3a.type = v3b.type = DataType::Vector3;
    blobA.type = blobB.type = DataType::Blob;
    if (theseed::core::compareProperty(v3a, v3b) != std::partial_ordering::unordered) {
        FAIL("Vector3 should be unordered"); return;
    }
    if (theseed::core::compareProperty(blobA, blobB) != std::partial_ordering::unordered) {
        FAIL("Blob should be unordered"); return;
    }
    // rawValue 尺寸损坏 → unordered（decodeNative 失败）
    auto broken = makeInt32Prop(4, "v", 7);
    broken.rawValue.pop_back();
    if (theseed::core::compareProperty(broken, lo) != std::partial_ordering::unordered) {
        FAIL("corrupted size should be unordered"); return;
    }
    // matchesFilter 对属性缺失/类型不符返回 false
    EntityData e;
    e.properties.push_back(lo);
    QueryFilter strFilter = QueryFilter::of("v", QueryOp::Eq, std::string("x"));
    if (theseed::core::matchesFilter(e, strFilter)) { FAIL("string vs int should not match"); return; }
    QueryFilter missing = QueryFilter::of("nope", QueryOp::Eq, std::int32_t(1));
    if (theseed::core::matchesFilter(e, missing)) { FAIL("missing property should not match"); return; }
    PASS();
}

// listIds 可见但 load 失败的实体应被 query 跳过而不是中断。
class LoadFailingStore final : public IEntityStore {
public:
    explicit LoadFailingStore(std::shared_ptr<IEntityStore> inner)
        : inner_(std::move(inner)) {}

    bool load(EntityId id, const std::string& entityType, EntityData& out) override {
        if (id == failId_) return false;
        return inner_->load(id, entityType, out);
    }
    bool save(EntityId id, const EntityData& data) override { return inner_->save(id, data); }
    bool remove(EntityId id) override { return inner_->remove(id); }
    EntityId allocId() override { return inner_->allocId(); }
    std::vector<EntityId> listIdsByType(const std::string& entityType) override {
        return inner_->listIdsByType(entityType);
    }
    std::vector<std::string> listEntityTypes() override { return inner_->listEntityTypes(); }

    EntityId failId_ = 0;

private:
    std::shared_ptr<IEntityStore> inner_;
};

static void test_query_skips_load_failures() {
    TEST("test_query_skips_load_failures");
    auto inner = seedStore();
    LoadFailingStore failing(inner);
    failing.failId_ = 2;  // #2 在 listIds 里可见但 load 失败
    InMemoryQueryStore qs(std::shared_ptr<IEntityStore>(&failing, [](IEntityStore*) {}));
    StorageQuery q;
    q.entityType = "Avatar";
    q.filters.push_back(QueryFilter::of("level", QueryOp::Ge, std::int32_t(0)));
    auto ids = qs.query(q);
    if (ids.size() != 4) { FAIL("expected 4 (skip failing #2), got " + std::to_string(ids.size())); return; }
    for (auto id : ids) {
        if (id == 2) { FAIL("failing id should not appear"); return; }
    }
    PASS();
}

int main() {
    test_query_int32_eq();
    test_query_int32_comparisons();
    test_query_uint64();
    test_query_float32();
    test_query_string();
    test_query_bool();
    test_query_and_filters();
    test_query_limit();
    test_query_offset();
    test_query_missing_property();
    test_query_type_mismatch();
    test_query_empty_entity_type();
    test_query_no_filter_matches_all();
    test_query_unknown_type();
    test_null_store_rejected();
    test_all_numeric_types_round_trip();
    test_filter_of_int64_and_double();
    test_compare_property_direct();
    test_query_skips_load_failures();

    std::cout << "  passed=" << testsPassed << " failed=" << testsFailed << "\n";
    return testsFailed == 0 ? 0 : 1;
}
