#include "theseed/core/EntityData.h"
#include "theseed/db/MySQLEntityStore.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>

using theseed::core::EntityData;
using theseed::core::PropertyData;
using theseed::core::DataType;
using theseed::db::MySQLEntityStore;

// 这些测试需要一个真实运行的 MySQL 实例。通过环境变量配置连接：
//   THESEED_MYSQL_HOST / THESEED_MYSQL_PORT / THESEED_MYSQL_USER /
//   THESEED_MYSQL_PASSWORD / THESEED_MYSQL_DATABASE
// 未设置 THESEED_MYSQL_HOST 时整体跳过（返回 0），CI 在无 MySQL 环境下不会失败。
//
// 运行示例：
//   THESEED_MYSQL_HOST=127.0.0.1 THESEED_MYSQL_USER=root \
//   THESEED_MYSQL_PASSWORD=secret THESEED_MYSQL_DATABASE=theseed_test \
//   ./theseed_mysql_store_test

namespace {

#define PASS() std::cout << "OK" << std::endl
#define FAIL(msg)                                                    \
    do {                                                             \
        std::cout << "FAILED: " << msg << std::endl;                 \
        return 1;                                                    \
    } while (0)

#define CHECK(cond, msg)                              \
    do {                                              \
        std::cout << "  " << msg << "... ";           \
        if (!(cond)) { FAIL(#cond " failed at " msg); } \
        PASS();                                       \
    } while (0)

MySQLEntityStore::Config configFromEnv() {
    MySQLEntityStore::Config cfg;
    if (const char* h = std::getenv("THESEED_MYSQL_HOST")) cfg.mysql.host = h;
    if (const char* p = std::getenv("THESEED_MYSQL_PORT"))
        cfg.mysql.port = static_cast<std::uint16_t>(std::atoi(p));
    if (const char* u = std::getenv("THESEED_MYSQL_USER")) cfg.mysql.user = u;
    if (const char* pw = std::getenv("THESEED_MYSQL_PASSWORD")) cfg.mysql.password = pw;
    if (const char* db = std::getenv("THESEED_MYSQL_DATABASE")) cfg.mysql.database = db;
    cfg.autoCreateSchema = true;
    return cfg;
}

// 测试用的 Avatar 实体：与 res/entities/Avatar.xml 同结构。
EntityData makeAvatar(theseed::core::EntityId id, std::int32_t level, float hp, float x) {
    EntityData data;
    data.id = id;
    data.entityType = "Avatar";

    auto addNumeric = [&](std::uint32_t pid, const std::string& name, DataType type,
                          const void* value, std::size_t size) {
        PropertyData p;
        p.id = pid;
        p.name = name;
        p.type = type;
        p.rawValue.resize(size);
        std::memcpy(p.rawValue.data(), value, size);
        data.properties.push_back(std::move(p));
    };

    addNumeric(0, "level", DataType::Int32, &level, sizeof(level));
    addNumeric(1, "hp", DataType::Float32, &hp, sizeof(hp));
    addNumeric(2, "x", DataType::Float32, &x, sizeof(x));
    return data;
}

}  // namespace

int main() {
    if (std::getenv("THESEED_MYSQL_HOST") == nullptr) {
        std::cout << "MySQLEntityStoreTest: skipped (set THESEED_MYSQL_HOST to enable)\n";
        return 0;
    }

    MySQLEntityStore store(configFromEnv());
    if (!store.init()) {
        std::cerr << "MySQLEntityStore init failed: " << store.lastError() << std::endl;
        return 1;
    }
    std::cout << "MySQLEntityStore connected\n";

    // 清表，保证测试隔离
    store.executeRaw("DELETE FROM tbl_Avatar");
    store.executeRaw("DELETE FROM tbl_Account");
    store.executeRaw("DELETE FROM _account_index");
    store.executeRaw("DELETE FROM _entity_ids");

    // --- save + load 往返 ---
    {
        auto avatar = makeAvatar(1, 42, 99.5f, 12.5f);
        CHECK(store.save(1, avatar), "save avatar#1");

        EntityData loaded;
        CHECK(store.load(1, "Avatar", loaded), "load avatar#1");
        CHECK(loaded.id == 1, "loaded id matches");
        CHECK(loaded.entityType == "Avatar", "loaded type matches");

        const auto* level = loaded.findPropertyByName("level");
        std::int32_t lv = 0;
        std::memcpy(&lv, level->rawValue.data(), sizeof(lv));
        CHECK(lv == 42, "level round-trips through BLOB");

        const auto* hp = loaded.findPropertyByName("hp");
        float hpVal = 0;
        std::memcpy(&hpVal, hp->rawValue.data(), sizeof(hpVal));
        CHECK(hpVal == 99.5f, "hp round-trips");
    }

    // --- save 覆盖（ON DUPLICATE KEY UPDATE）---
    {
        auto updated = makeAvatar(1, 100, 50.0f, 0.0f);
        CHECK(store.save(1, updated), "save avatar#1 update");
        EntityData loaded;
        store.load(1, "Avatar", loaded);
        const auto* level = loaded.findPropertyByName("level");
        std::int32_t lv = 0;
        std::memcpy(&lv, level->rawValue.data(), sizeof(lv));
        CHECK(lv == 100, "save overwrites existing row");
    }

    // --- listIdsByType / listEntityTypes ---
    {
        store.save(2, makeAvatar(2, 1, 1.0f, 2.0f));
        auto ids = store.listIdsByType("Avatar");
        CHECK(ids.size() == 2, "listIdsByType returns 2 avatars");
        CHECK(ids[0] == 1 && ids[1] == 2, "ids sorted ascending");

        auto types = store.listEntityTypes();
        bool foundAvatar = false;
        for (const auto& t : types) if (t == "Avatar") foundAvatar = true;
        CHECK(foundAvatar, "listEntityTypes includes Avatar");
    }

    // --- remove ---
    {
        CHECK(store.remove(2), "remove avatar#2");
        EntityData loaded;
        CHECK(!store.load(2, "Avatar", loaded), "removed entity no longer loads");
    }

    // --- allocId 原子递增 ---
    {
        auto id1 = store.allocId();
        auto id2 = store.allocId();
        CHECK(id1 != 0 && id2 != 0, "allocId returns nonzero");
        CHECK(id2 == id1 + 1, "allocId increments atomically");
    }

    // --- account 索引查询 ---
    {
        theseed::core::EntityId newId = 0;
        CHECK(store.createAccount("alice", "secret", newId), "createAccount alice");
        CHECK(newId != 0, "createAccount returns id");

        // 重复创建失败
        theseed::core::EntityId dup = 0;
        CHECK(!store.createAccount("alice", "x", dup), "duplicate username rejected");

        // 查询命中
        theseed::core::EntityId qid = 0;
        std::string pwd;
        CHECK(store.queryAccount("alice", qid, pwd), "queryAccount alice found");
        CHECK(qid == newId, "queryAccount id matches");
        CHECK(pwd == "secret", "queryAccount password matches");

        // 查询未命中
        theseed::core::EntityId miss = 999;
        std::string missPwd;
        CHECK(!store.queryAccount("nobody", miss, missPwd), "queryAccount miss returns false");
    }

    std::cout << "\nMySQLEntityStoreTest: all passed\n";
    return 0;
}
