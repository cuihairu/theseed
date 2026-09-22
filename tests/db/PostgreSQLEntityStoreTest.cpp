#include "theseed/core/EntityData.h"
#include "theseed/db/PostgreSQLEntityStore.h"

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
using theseed::db::PostgreSQLEntityStore;

// 这些测试需要一个真实运行的 PostgreSQL 实例。通过环境变量配置连接：
//   THESEED_PG_HOST / THESEED_PG_PORT / THESEED_PG_USER /
//   THESEED_PG_PASSWORD / THESEED_PG_DATABASE
// 未设置 THESEED_PG_HOST 时整体跳过（返回 0），CI 在无 PostgreSQL 环境下不会失败。
//
// 本地 podman 运行示例：
//   podman run -d --name theseed-postgres -e POSTGRES_PASSWORD=theseed_test_pw
//     -e POSTGRES_DB=theseed_test -p 127.0.0.1:13307:5432 docker.io/library/postgres:16
//
//   THESEED_PG_HOST=127.0.0.1 THESEED_PG_PORT=13307 THESEED_PG_USER=postgres
//   THESEED_PG_PASSWORD=theseed_test_pw THESEED_PG_DATABASE=theseed_test
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

    const auto envCfg = pgConfigFromEnv();

    // --- sanitize：非法字符映射为下划线，合法字符原样保留 ---
    {
        auto weird = makeAvatar(10, 1, 1.0f, 2.0f);
        weird.entityType = "P1-yer z";
        CHECK(store.save(10, weird), "save dirty type name");
        EntityData loaded;
        CHECK(store.load(10, "P1-yer z", loaded), "load dirty type name");
        CHECK(loaded.entityType == "P1-yer z", "dirty type round-trips");
        store.executeRaw("DROP TABLE IF EXISTS \"tbl_P1_yer_z\"");
    }

    // --- executeRaw 拒绝坏 SQL ---
    CHECK(!store.executeRaw("THIS IS NOT VALID SQL"), "executeRaw bad sql");

    // --- 未 init 的 store：操作报 not initialized ---
    {
        PostgreSQLEntityStore ghost(PostgreSQLEntityStore::Config{});
        EntityData loaded;
        CHECK(!ghost.load(1, "Avatar", loaded), "ghost load rejected");
        CHECK(ghost.lastError() == "store not initialized", "ghost error text");
    }

    // --- 坏端口：init 失败并带出连接错误 ---
    {
        auto bad = pgConfigFromEnv();
        bad.port = 1;
        PostgreSQLEntityStore::Config badCfg;
        badCfg.pg = bad;
        badCfg.autoCreateSchema = true;
        PostgreSQLEntityStore refused(std::move(badCfg));
        CHECK(!refused.init(), "init with dead port fails");
        CHECK(refused.lastError().find("connect failed") != std::string::npos,
              "connect error surfaced");
    }

    // --- 只读用户：connect 成功但 createSchema 的 DDL、ensureTable 被拒 ---
    {
        theseed::db::PostgreSQLConnection admin(envCfg);
        CHECK(admin.connect(), "admin connect for ro user");
        // 上次运行可能残留同名角色（授权依赖会阻止直接 DROP），先清依赖
        admin.execute("DROP OWNED BY ro_cov");
        admin.execute("DROP ROLE IF EXISTS ro_cov");
        CHECK(admin.execute("CREATE USER ro_cov WITH PASSWORD 'cov_pw'"),
              "create ro user");
        CHECK(admin.execute("GRANT USAGE ON SCHEMA public TO ro_cov"),
              "grant ro schema usage");
        CHECK(admin.execute("GRANT SELECT ON ALL TABLES IN SCHEMA public TO ro_cov"),
              "grant ro select");

        auto ro = pgConfigFromEnv();
        ro.user = "ro_cov";
        ro.password = "cov_pw";
        {
            // autoCreateSchema=true：init 在 create _entity_ids 处被拒
            PostgreSQLEntityStore::Config roSchema;
            roSchema.pg = ro;
            roSchema.autoCreateSchema = true;
            PostgreSQLEntityStore readonly(roSchema);
            CHECK(!readonly.init(), "ro init denied at createSchema");
            CHECK(readonly.lastError().find("_entity_ids") != std::string::npos,
                  "ro error mentions _entity_ids");
        }
        {
            // autoCreateSchema=false：init 成功，但 save 的 ensureTable 被拒
            PostgreSQLEntityStore::Config roPlain;
            roPlain.pg = ro;
            roPlain.autoCreateSchema = false;
            PostgreSQLEntityStore readonly(roPlain);
            CHECK(readonly.init(), "ro init without schema");
            auto avatar = makeAvatar(1, 1, 1.0f, 0.0f);
            CHECK(!readonly.save(1, avatar), "ro save denied");
            CHECK(readonly.lastError().find("ensureTable failed") != std::string::npos,
                  "ensureTable error surfaced");
        }

        admin.execute("DROP OWNED BY ro_cov");
        admin.execute("DROP ROLE IF EXISTS ro_cov");
    }

    // --- config.connection 注入 + 表锁超时路径 ---
    // 注入连接设 lock_timeout=1s：被 ACCESS EXCLUSIVE 锁阻塞的语句 1 秒即报错。
    {
        auto cfg = pgConfigFromEnv();
        auto conn = std::make_shared<theseed::db::PostgreSQLConnection>(cfg);
        CHECK(conn->connect(), "shared conn connect");
        conn->execute("SET lock_timeout = '1s'");
        PostgreSQLEntityStore::Config injCfg;
        injCfg.pg = cfg;
        injCfg.autoCreateSchema = false;
        injCfg.connection = conn;
        PostgreSQLEntityStore injected(std::move(injCfg));
        CHECK(injected.init(), "init with injected connection");

        // 先让 tbl_Avatar / tbl_Lt 进入 knownTables_ 缓存，锁测试只等一次
        CHECK(injected.save(1, makeAvatar(1, 1, 1.0f, 0.0f)), "injected save avatar#1");
        auto lt = makeAvatar(12, 1, 1.0f, 2.0f);
        lt.entityType = "Lt";
        CHECK(injected.save(12, lt), "injected save Lt");

        theseed::db::PostgreSQLConnection locker(cfg);
        CHECK(locker.connect(), "locker connect");

        // save：tbl_Lt 被锁 → INSERT 等锁超时
        CHECK(locker.execute("BEGIN"), "begin tx");
        CHECK(locker.execute("LOCK TABLE \"tbl_Lt\" IN ACCESS EXCLUSIVE MODE"),
              "lock tbl_Lt");
        CHECK(!injected.save(12, lt), "save blocked by lock");
        CHECK(locker.execute("ROLLBACK"), "unlock tbl_Lt");

        // load：tbl_Avatar 被锁 → SELECT 等锁超时
        CHECK(locker.execute("BEGIN"), "begin tx");
        CHECK(locker.execute("LOCK TABLE \"tbl_Avatar\" IN ACCESS EXCLUSIVE MODE"),
              "lock tbl_Avatar");
        {
            EntityData loaded;
            CHECK(!injected.load(1, "Avatar", loaded), "load blocked by lock");
        }
        CHECK(locker.execute("ROLLBACK"), "unlock tbl_Avatar");

        // remove：tbl_Avatar 被锁 → DELETE 等锁超时
        CHECK(locker.execute("BEGIN"), "begin tx");
        CHECK(locker.execute("LOCK TABLE \"tbl_Avatar\" IN ACCESS EXCLUSIVE MODE"),
              "lock tbl_Avatar again");
        CHECK(!injected.remove(1), "remove blocked by lock");
        CHECK(locker.execute("ROLLBACK"), "unlock tbl_Avatar again");

        // allocId：_entity_ids 被锁 → INSERT...RETURNING 等锁超时
        CHECK(locker.execute("BEGIN"), "begin tx");
        CHECK(locker.execute("LOCK TABLE _entity_ids IN ACCESS EXCLUSIVE MODE"),
              "lock _entity_ids");
        CHECK(injected.allocId() == 0, "allocId blocked by lock");
        CHECK(locker.execute("ROLLBACK"), "unlock _entity_ids");

        // createAccount：_account_index 被锁 → 查重失败视为不存在 →
        // allocId 与主表写入成功 → 索引 INSERT 等锁超时
        CHECK(locker.execute("BEGIN"), "begin tx");
        CHECK(locker.execute("LOCK TABLE _account_index IN ACCESS EXCLUSIVE MODE"),
              "lock _account_index");
        {
            theseed::core::EntityId blockedId = 0;
            CHECK(!injected.createAccount("locked_out", "pw", blockedId),
                  "createAccount blocked by lock");
        }
        CHECK(locker.execute("ROLLBACK"), "unlock _account_index");

        injected.executeRaw("DROP TABLE IF EXISTS \"tbl_Lt\"");
    }

    // --- createSchema / ensureTable 失败路径：用同名对象占位拦截 DDL ---
    {
        auto cfg = pgConfigFromEnv();

        // 同名 view 占住 _account_index：CREATE TABLE IF NOT EXISTS 遇到
        // 非表对象会直接报错（IF NOT EXISTS 只对同型对象跳过）
        {
            theseed::db::PostgreSQLConnection admin(cfg);
            CHECK(admin.connect(), "admin connect (view trap)");
            admin.execute("DROP VIEW IF EXISTS _account_index");
            admin.execute("DROP TABLE IF EXISTS _account_index");
            CHECK(admin.execute("CREATE VIEW _account_index AS SELECT 1"),
                  "create trap view");
            PostgreSQLEntityStore::Config trap;
            trap.pg = cfg;
            {
                PostgreSQLEntityStore store(std::move(trap));
                CHECK(!store.init(), "init fails when _account_index is a view");
                CHECK(store.lastError().find("_account_index") != std::string::npos,
                      "error mentions _account_index");
            }
            CHECK(admin.execute("DROP VIEW _account_index"), "drop trap view");
            PostgreSQLEntityStore::Config repair;
            repair.pg = cfg;
            PostgreSQLEntityStore store(std::move(repair));
            CHECK(store.init(), "repair schema after view trap");
        }

        // 同名但缺列的表：CREATE TABLE IF NOT EXISTS 静默跳过，而
        // CREATE INDEX ... (entity_id) 因列不存在报错
        {
            theseed::db::PostgreSQLConnection admin(cfg);
            CHECK(admin.connect(), "admin connect (index trap)");
            admin.execute("DROP INDEX IF EXISTS idx_account_entity_id");
            admin.execute("DROP TABLE IF EXISTS idx_account_entity_id");
            admin.execute("DROP TABLE IF EXISTS _account_index");
            CHECK(admin.execute(
                      "CREATE TABLE _account_index ("
                      "  username VARCHAR(128) NOT NULL PRIMARY KEY,"
                      "  password BYTEA NOT NULL)"),
                  "create entity_id-less trap table");
            PostgreSQLEntityStore::Config trap;
            trap.pg = cfg;
            PostgreSQLEntityStore store(std::move(trap));
            CHECK(!store.init(), "init fails when index column missing");
            // createSchema 的 index 分支前缀是 "create _account_index index failed"
            CHECK(store.lastError().find("index failed") != std::string::npos,
                  "error mentions index step");
            CHECK(admin.execute("DROP TABLE _account_index"), "drop trap table");
            PostgreSQLEntityStore::Config repair;
            repair.pg = cfg;
            PostgreSQLEntityStore repaired(std::move(repair));
            CHECK(repaired.init(), "repair schema after index trap");
        }

        // 同名但缺 updated_at 列的实体表：save 走 ensureTable 的
        // CREATE INDEX ON (updated_at) 失败
        {
            theseed::db::PostgreSQLConnection admin(cfg);
            CHECK(admin.connect(), "admin connect (ensureTable trap)");
            admin.execute("DROP TABLE IF EXISTS \"idx_updated_tbl_Cov\"");
            admin.execute("DROP TABLE IF EXISTS \"tbl_Cov\"");
            CHECK(admin.execute(
                      "CREATE TABLE \"tbl_Cov\" ("
                      "  id BIGINT NOT NULL PRIMARY KEY,"
                      "  data BYTEA NOT NULL)"),
                  "create updated_at-less trap table");
            PostgreSQLEntityStore::Config trap;
            trap.pg = cfg;
            PostgreSQLEntityStore store(std::move(trap));
            CHECK(store.init(), "init ok before ensureTable trap");
            EntityData cov;
            cov.entityType = "Cov";
            CHECK(!store.save(1, cov), "save fails when ensureTable index column missing");
            CHECK(store.lastError().find("index failed") != std::string::npos,
                  "error mentions ensureTable index step");
            CHECK(admin.execute("DROP TABLE \"tbl_Cov\""),
                  "drop ensureTable trap table");
        }
    }

    std::cout << std::endl << "PostgreSQLEntityStoreTest: all passed" << std::endl;
    return 0;
}
