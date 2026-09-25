#include "theseed/core/EntityData.h"
#include "theseed/core/IEntityStore.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using theseed::core::DataType;
using theseed::core::EntityData;
using theseed::core::InMemoryEntityStore;
using theseed::core::PropertyData;

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

static void testPropertyDataFixedSize() {
    TEST("PropertyData::fixedSizeOfType");

    bool ok = true;
    ok = ok && PropertyData::fixedSizeOfType(DataType::Int8) == 1;
    ok = ok && PropertyData::fixedSizeOfType(DataType::Int32) == 4;
    ok = ok && PropertyData::fixedSizeOfType(DataType::Float64) == 8;
    ok = ok && PropertyData::fixedSizeOfType(DataType::Bool) == 1;
    ok = ok && PropertyData::fixedSizeOfType(DataType::Vector3) == 12;
    ok = ok && PropertyData::fixedSizeOfType(DataType::String) == 0;
    ok = ok && PropertyData::fixedSizeOfType(DataType::Blob) == 0;

    if (ok) PASS(); else FAIL("size mismatch");
}

static void testPropertyDataVariableSized() {
    TEST("PropertyData::isVariableSized");

    bool ok = PropertyData::isVariableSized(DataType::String);
    ok = ok && PropertyData::isVariableSized(DataType::Blob);
    ok = ok && !PropertyData::isVariableSized(DataType::Int32);
    ok = ok && !PropertyData::isVariableSized(DataType::Vector3);

    if (ok) PASS(); else FAIL("wrong classification");
}

static void testEntityDataFindProperty() {
    TEST("EntityData::findProperty");

    EntityData data;
    data.properties.push_back({0, "level", DataType::Int32, {}});
    data.properties.push_back({1, "name", DataType::String, {}});

    bool ok = data.findProperty(0) != nullptr;
    ok = ok && data.findProperty(0)->name == "level";
    ok = ok && data.findProperty(1)->name == "name";
    ok = ok && data.findProperty(99) == nullptr;
    ok = ok && data.findPropertyByName("name") != nullptr;
    ok = ok && data.findPropertyByName("name")->id == 1;
    ok = ok && data.findPropertyByName("missing") == nullptr;

    // const 上下文走 const 重载
    const auto& cdata = data;
    ok = ok && cdata.findProperty(0) != nullptr;
    ok = ok && cdata.findProperty(0)->name == "level";
    ok = ok && cdata.findProperty(99) == nullptr;
    ok = ok && cdata.findPropertyByName("missing") == nullptr;

    if (ok) PASS(); else FAIL("find failed");
}

// 截断的属性数据流：decodeProperty 内部抛异常 → decodeEntityData 捕获返回 false
static void testDecodeTruncatedProperty() {
    TEST("decodeEntityData rejects truncated property stream");

    EntityData original;
    original.id = 7;
    original.entityType = "Avatar";
    PropertyData prop;
    prop.id = 0;
    prop.name = "level";
    prop.type = DataType::Int32;
    prop.rawValue.resize(4);
    original.properties.push_back(prop);

    theseed::core::MemoryStream ms;
    theseed::core::encodeEntityData(ms, original);
    // 从尾部截掉若干字节 → 最后一个属性解码必然失败
    bool ok = true;
    for (std::size_t cut = 1; cut <= 6 && ok; ++cut) {
        theseed::core::MemoryStream in;
        in.writeBytes(ms.data(), ms.size() - cut);
        in.resetRead();
        EntityData out;
        if (theseed::core::decodeEntityData(in, out)) {
            ok = false;  // 不应有任何截断量能解码成功
        }
    }

    if (ok) PASS(); else FAIL("truncated stream unexpectedly decoded");
}

static void testEntityDataEncodeDecode() {
    TEST("EntityData encode/decode roundtrip");

    EntityData original;
    original.id = 42;
    original.entityType = "Avatar";

    PropertyData prop1;
    prop1.id = 0;
    prop1.name = "level";
    prop1.type = DataType::Int32;
    std::int32_t levelVal = 99;
    prop1.rawValue.resize(4);
    std::memcpy(prop1.rawValue.data(), &levelVal, 4);

    PropertyData prop2;
    prop2.id = 1;
    prop2.name = "name";
    prop2.type = DataType::String;
    const std::string nameStr = "hero";
    prop2.rawValue.assign(
        reinterpret_cast<const std::byte*>(nameStr.data()),
        reinterpret_cast<const std::byte*>(nameStr.data() + nameStr.size()));

    original.properties.push_back(std::move(prop1));
    original.properties.push_back(std::move(prop2));

    theseed::core::MemoryStream stream;
    theseed::core::encodeEntityData(stream, original);

    stream.resetRead();
    EntityData decoded;
    bool ok = theseed::core::decodeEntityData(stream, decoded);
    ok = ok && decoded.id == 42;
    ok = ok && decoded.entityType == "Avatar";
    ok = ok && decoded.properties.size() == 2;
    ok = ok && decoded.properties[0].name == "level";
    ok = ok && decoded.properties[1].name == "name";

    std::int32_t decodedLevel = 0;
    std::memcpy(&decodedLevel, decoded.properties[0].rawValue.data(), 4);
    ok = ok && decodedLevel == 99;

    if (ok) PASS(); else FAIL("roundtrip failed");
}

static void testInMemoryStoreSaveLoad() {
    TEST("InMemoryEntityStore save/load");

    InMemoryEntityStore store;

    EntityData data;
    data.id = store.allocId();
    data.entityType = "Player";
    PropertyData prop;
    prop.id = 0;
    prop.name = "score";
    prop.type = DataType::Int32;
    std::int32_t score = 1000;
    prop.rawValue.resize(4);
    std::memcpy(prop.rawValue.data(), &score, 4);
    data.properties.push_back(std::move(prop));

    bool ok = store.save(data.id, data);
    ok = ok && store.exists(data.id);
    ok = ok && store.count() == 1;

    EntityData loaded;
    ok = ok && store.load(data.id, "Player", loaded);
    ok = ok && loaded.id == data.id;
    ok = ok && loaded.entityType == "Player";
    ok = ok && loaded.properties.size() == 1;

    std::int32_t loadedScore = 0;
    std::memcpy(&loadedScore, loaded.properties[0].rawValue.data(), 4);
    ok = ok && loadedScore == 1000;

    if (ok) PASS(); else FAIL("save/load failed");
}

static void testInMemoryStoreListEntityTypes() {
    TEST("InMemoryEntityStore listEntityTypes");

    InMemoryEntityStore store;
    EntityData a;
    a.id = store.allocId();
    a.entityType = "Player";
    EntityData b;
    b.id = store.allocId();
    b.entityType = "Monster";
    EntityData c;
    c.id = store.allocId();
    c.entityType = "Player";

    bool ok = store.save(a.id, a) && store.save(b.id, b) && store.save(c.id, c);

    // 返回值按字母序去重
    const auto types = store.listEntityTypes();
    ok = ok && types.size() == 2;
    ok = ok && types[0] == "Monster" && types[1] == "Player";

    const auto players = store.listIdsByType("Player");
    ok = ok && players.size() == 2;

    if (ok) PASS(); else FAIL("listEntityTypes failed");
}

static void testInMemoryStoreRemove() {
    TEST("InMemoryEntityStore remove");

    InMemoryEntityStore store;

    EntityData data;
    data.id = store.allocId();
    data.entityType = "Item";

    bool ok = store.save(data.id, data);
    ok = ok && store.exists(data.id);

    ok = ok && store.remove(data.id);
    ok = ok && !store.exists(data.id);
    ok = ok && !store.remove(data.id);  // already removed

    if (ok) PASS(); else FAIL("remove failed");
}

static void testInMemoryStoreLoadMissing() {
    TEST("InMemoryEntityStore load missing");

    InMemoryEntityStore store;
    EntityData data;

    bool ok = !store.load(999, "Missing", data);

    if (ok) PASS(); else FAIL("should return false");
}

static void testInMemoryStoreAllocId() {
    TEST("InMemoryEntityStore allocId");

    InMemoryEntityStore store;
    auto id1 = store.allocId();
    auto id2 = store.allocId();
    auto id3 = store.allocId();

    bool ok = id1 == 1 && id2 == 2 && id3 == 3;

    if (ok) PASS(); else FAIL("id sequence wrong");
}

// 分支覆盖：encode 的空 rawValue 跳过臂、decode 的 size==0 跳过臂、
// count 超限防御臂与空属性列表边界。
static void testEncodeDecodeEdgeCases() {
    TEST("EntityData encode/decode edge cases");

    bool ok = true;

    // 空 String 属性：encode 跳过 writeBytes（rawValue 空）、decode size==0 跳过 readBytes
    {
        EntityData original;
        original.id = 1;
        original.entityType = "Ghost";
        PropertyData emptyStr;
        emptyStr.id = 0;
        emptyStr.name = "nick";
        emptyStr.type = DataType::String;
        // rawValue 保持为空
        original.properties.push_back(emptyStr);

        theseed::core::MemoryStream stream;
        theseed::core::encodeEntityData(stream, original);
        stream.resetRead();
        EntityData decoded;
        ok = ok && theseed::core::decodeEntityData(stream, decoded);
        ok = ok && decoded.properties.size() == 1;
        ok = ok && decoded.properties[0].name == "nick";
        ok = ok && decoded.properties[0].rawValue.empty();
    }

    // 空属性列表：count=0 往返
    {
        EntityData original;
        original.id = 2;
        original.entityType = "Empty";
        theseed::core::MemoryStream stream;
        theseed::core::encodeEntityData(stream, original);
        stream.resetRead();
        EntityData decoded;
        ok = ok && theseed::core::decodeEntityData(stream, decoded);
        ok = ok && decoded.properties.empty();
    }

    // 垃圾 count 超出合理上限：防御臂直接拒绝（不触发超大分配）
    {
        theseed::core::MemoryStream evil;
        evil.writeUint64(3);
        evil.writeString("Evil");
        evil.writeUint32((1u << 20) + 1);  // kMaxPropertyCount + 1
        evil.resetRead();
        EntityData out;
        ok = ok && !theseed::core::decodeEntityData(evil, out);
    }

    if (ok) PASS();
    else FAIL("encode/decode edge case failed");
}

int main() {
    std::cout << "EntityData tests:\n";

    testPropertyDataFixedSize();
    testPropertyDataVariableSized();
    testEntityDataFindProperty();
    testDecodeTruncatedProperty();
    testEntityDataEncodeDecode();
    testEncodeDecodeEdgeCases();
    testInMemoryStoreSaveLoad();
    testInMemoryStoreRemove();
    testInMemoryStoreListEntityTypes();
    testInMemoryStoreLoadMissing();
    testInMemoryStoreAllocId();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
