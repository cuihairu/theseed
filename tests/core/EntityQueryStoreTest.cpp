#include "theseed/core/EntityData.h"
#include "theseed/core/EntityQuery.h"
#include "theseed/core/IEntityStore.h"

#include <cstring>
#include <iostream>
#include <memory>
#include <string>

using theseed::core::DataType;
using theseed::core::EntityData;
using theseed::core::EntityId;
using theseed::core::IEntityQueryStore;
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

    std::cout << "  passed=" << testsPassed << " failed=" << testsFailed << "\n";
    return testsFailed == 0 ? 0 : 1;
}
