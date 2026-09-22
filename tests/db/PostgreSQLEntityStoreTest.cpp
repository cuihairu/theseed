#include "theseed/core/EntityData.h"
#include "theseed/db/PostgreSQLEntityStore.h"

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>

using theseed::core::EntityData;
using theseed::core::PropertyData;
using theseed::core::DataType;
using theseed::db::PostgreSQLEntityStore;

// 这些测试需要一个真实运行的 PostgreSQL 实例。通过环境变量配置连接：
//   THESEED_PG_HOST / THESEED_PG_PORT / THESEED_PG_USER /
//   THESEED_PG_PASSWORD / THESEED_PG_DATABASE
// 未设置 THESEED_PG_HOST 时整体跳过（返回 0），CI 在无 PostgreSQL 环境下不会失败。
//
// 本地 podman 运行示例：
//   podman run -d --name theseed-postgres -e POSTGRES_PASSWORD=theseed_test_pw \
//     -e POSTGRES_DB=theseed_test -p 127.0.0.1:13307:5432 docker.io/library/postgres:16
//
//   THESEED_PG_HOST=127.0.0.1 THESEED_PG_PORT=13307 THESEED_PG_USER=postgres \
//   THESEED_PG_PASSWORD=theseed_test_pw THESEED_PG_DATABASE=theseed_test \
//   ./theseed_pg_store_test

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

theseed::db::PostgreSQLConnectionConfig pgConfigFromEnv() {
    theseed::db::PostgreSQLConnectionConfig cfg;
    if (const char* h = std::getenv("THESEED_PG_HOST")) cfg.host = h;
    if (const char* p = std::getenv("THESEED_PG_PORT"))
        cfg.port = static_cast<std::uint16_t>(std::atoi(p));
    if (const char* u = std::getenv("THESEED_PG_USER")) cfg.user = u;
    if (const char* pw = std::getenv("THESEED_PG_PASSWORD")) cfg.password = pw;
    if (const char* db = std::getenv("THESEED_PG_DATABASE")) cfg.database = db;
    return cfg;
}

// 测试用的 Avatar 实体：与 res/entities/Avatar.xml 同结构。
EntityData makeAvatar(theseed::core::EntityId id, std::int32_t level, float hp, float x) {
    EntityData data;
    data.id = id;
    data.entityType = "Avatar";

    auto addNumeric = [&](std::uint32_t pid, const std::string& name, DataType type,
                          const void* value, std::size_t size) {
        PropertyData prop;
        prop.id = pid;
        prop.name = name;
        prop.type = type;
        const auto* bytes = static_cast<const std::byte*>(value);
        prop.rawValue.assign(bytes, bytes + size);
        data.properties.push_back(std::move(prop));
    };

    addNumeric(0, "level", DataType::Int32, &level, sizeof(level));
    addNumeric(1, "hp", DataType::Float32, &hp, sizeof(hp));
    addNumeric(2, "x", DataType::Float32, &x, sizeof(x));
    return data;
}

}  // namespace

int main() {
    if (std::getenv("THESEED_PG_HOST") == nullptr) {
        std::cout << "THESEED_PG_HOST not set, skipping PostgreSQLEntityStoreTest"
                  << std::endl;
        return 0;
    }

    PostgreSQLEntityStore::Config cfg;
    cfg.pg = pgConfigFromEnv();
    cfg.autoCreateSchema = true;
    PostgreSQLEntityStore store(std::move(cfg));

    if (!store.init()) {
        std::cout << "FAILED: store init: " << store.lastError() << std::endl;
        return 1;
    }
    std::cout << "PostgreSQLEntityStore connected" << std::endl;

    // 自隔离：清掉历史数据（如 DBApp E2E 留下的行），行数断言才有意义。
    // 表名含大写字母（建表带引号），DELETE 必须同样加引号。
    store.executeRaw("DELETE FROM \"tbl_Avatar\"");
    store.executeRaw("DELETE FROM \"tbl_Account\"");
    store.executeRaw("DELETE FROM \"_account_index\"");
    store.executeRaw("DELETE FROM \"_entity_ids\"");

    // --- save / load 往返 ---
    {
        auto avatar = makeAvatar(1, 42, 1234.5f, 7.25f);
        CHECK(store.save(1, avatar), "save avatar#1");
    }
    {
        EntityData out;
        CHECK(store.load(1, "Avatar", out), "load avatar#1");
        CHECK(out.id == 1, "loaded id matches");
        CHECK(out.entityType == "Avatar", "loaded type matches");

        bool levelOk = false, hpOk = false;
        for (const auto& p : out.properties) {
            if (p.name == "level" && p.type == DataType::Int32 &&
                p.rawValue.size() == sizeof(std::int32_t)) {
                std::int32_t lv = 0;
                std::memcpy(&lv, p.rawValue.data(), sizeof(lv));
                levelOk = (lv == 42);
            }
            if (p.name == "hp" && p.type == DataType::Float32 &&
                p.rawValue.size() == sizeof(float)) {
                float hv = 0;
                std::memcpy(&hv, p.rawValue.data(), sizeof(hv));
                hpOk = (hv == 1234.5f);
            }
        }
        CHECK(levelOk, "level round-trips through BYTEA");
        CHECK(hpOk, "hp round-trips");
    }

    // --- update 覆盖 ---
    {
        auto avatar = makeAvatar(1, 43, 2000.0f, 9.5f);
        CHECK(store.save(1, avatar), "save avatar#1 update");
        EntityData out;
        CHECK(store.load(1, "Avatar", out), "reload avatar#1");
        bool levelOk = false;
        for (const auto& p : out.properties) {
            if (p.name == "level" && p.type == DataType::Int32 &&
                p.rawValue.size() == sizeof(std::int32_t)) {
                std::int32_t lv = 0;
                std::memcpy(&lv, p.rawValue.data(), sizeof(lv));
                levelOk = (lv == 43);
            }
        }
        CHECK(levelOk, "save overwrites existing row");
    }

    // --- 第二个实体 + 列表 ---
    {
        auto avatar = makeAvatar(2, 1, 100.0f, 0.0f);
        CHECK(store.save(2, avatar), "save avatar#2");
        auto ids = store.listIdsByType("Avatar");
        CHECK(ids.size() == 2, "listIdsByType returns 2 avatars");
        CHECK(ids[0] == 1 && ids[1] == 2, "ids sorted ascending");
        auto types = store.listEntityTypes();
        bool hasAvatar = false;
        for (const auto& t : types) {
            if (t == "Avatar") hasAvatar = true;
        }
        CHECK(hasAvatar, "listEntityTypes includes Avatar");
    }

    // --- remove ---
    {
        CHECK(store.remove(2), "remove avatar#2");
        EntityData out;
        CHECK(!store.load(2, "Avatar", out), "removed entity no longer loads");
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

        theseed::core::EntityId dup = 0;
        CHECK(!store.createAccount("alice", "x", dup), "duplicate username rejected");

        theseed::core::EntityId id = 0;
        std::string password;
        CHECK(store.queryAccount("alice", id, password), "queryAccount alice found");
        CHECK(id == newId, "queryAccount id matches");
        CHECK(password == "secret", "queryAccount password matches");

        CHECK(!store.queryAccount("nobody", id, password), "queryAccount miss returns false");
    }

    std::cout << std::endl << "PostgreSQLEntityStoreTest: all passed" << std::endl;
    return 0;
}
