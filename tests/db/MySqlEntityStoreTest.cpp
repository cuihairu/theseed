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
//   THESEED_MYSQL_HOST=127.0.0.1 THESEED_MYSQL_USER=root
//   THESEED_MYSQL_PASSWORD=secret THESEED_MYSQL_DATABASE=theseed_test
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

    const auto envCfg = configFromEnv();

    // --- sanitize：非法字符映射为下划线，合法字符原样保留 ---
    {
        auto weird = makeAvatar(10, 1, 1.0f, 2.0f);
        weird.entityType = "P1-yer z";
        CHECK(store.save(10, weird), "save dirty type name");
        EntityData loaded;
        CHECK(store.load(10, "P1-yer z", loaded), "load dirty type name");
        CHECK(loaded.entityType == "P1-yer z", "dirty type round-trips");
        store.executeRaw("DROP TABLE IF EXISTS `tbl_P1_yer_z`");
    }

    // --- load 不存在的 id 返回 false 且不置错误，lastError 可读 ---
    {
        EntityData loaded;
        CHECK(!store.load(424242, "Avatar", loaded), "load missing id");
        CHECK(store.lastError().empty(), "missing id leaves no error");
    }

    // --- executeRaw 拒绝坏 SQL ---
    CHECK(!store.executeRaw("THIS IS NOT VALID SQL"), "executeRaw bad sql");

    // --- 未 init 的 store：操作报 not initialized ---
    {
        MySQLEntityStore ghost(MySQLEntityStore::Config{});
        EntityData loaded;
        CHECK(!ghost.load(1, "Avatar", loaded), "ghost load rejected");
        CHECK(ghost.lastError() == "store not initialized", "ghost error text");
    }

    // --- 坏端口：init 失败并带出连接错误 ---
    {
        auto bad = configFromEnv();
        bad.mysql.port = 1;
        MySQLEntityStore refused(bad);
        CHECK(!refused.init(), "init with dead port fails");
        CHECK(refused.lastError().find("connect failed") != std::string::npos,
              "connect error surfaced");
    }

    // --- 超长 entityType：表名超 64 字符标识符上限，ensureTable DDL 失败 ---
    {
        auto big = makeAvatar(13, 1, 1.0f, 2.0f);
        big.entityType = std::string(80, 'A');
        CHECK(!store.save(13, big), "save long table name fails");
        CHECK(store.lastError().find("ensureTable failed") != std::string::npos,
              "ensureTable error surfaced");
    }

    // --- 只读用户：connect 成功但 createSchema 的 DDL 被拒 ---
    {
        theseed::db::MySQLConnection admin(envCfg.mysql);
        CHECK(admin.connect(), "admin connect for ro user");
        admin.execute("DROP USER IF EXISTS 'ro_cov'@'%'");
        CHECK(admin.execute("CREATE USER 'ro_cov'@'%' IDENTIFIED BY 'cov_pw'"),
              "create ro user");
        CHECK(admin.execute("GRANT SELECT ON `" + envCfg.mysql.database +
                            "`.* TO 'ro_cov'@'%'"),
              "grant ro select");

        auto ro = configFromEnv();
        ro.mysql.user = "ro_cov";
        ro.mysql.password = "cov_pw";
        {
            MySQLEntityStore readonly(ro);
            CHECK(!readonly.init(), "ro init denied at createSchema");
            CHECK(readonly.lastError().find("_entity_ids") != std::string::npos,
                  "ro error mentions _entity_ids");
        }

        admin.execute("DROP USER IF EXISTS 'ro_cov'@'%'");
    }

    // --- config.connection 注入 + 表锁超时路径 ---
    // 注入连接设 lock_wait_timeout=1：被 LOCK TABLES 阻塞的语句 1 秒即报错。
    {
        auto cfg = configFromEnv();
        auto conn = std::make_shared<theseed::db::MySQLConnection>(cfg.mysql);
        CHECK(conn->connect(), "shared conn connect");
        conn->execute("SET SESSION lock_wait_timeout = 1");
        cfg.connection = conn;
        MySQLEntityStore injected(cfg);
        CHECK(injected.init(), "init with injected connection");

        // 先让 tbl_Avatar / tbl_Lt 进入 knownTables_ 缓存，锁测试只等一次
        CHECK(injected.save(1, makeAvatar(1, 1, 1.0f, 0.0f)), "injected save avatar#1");
        auto lt = makeAvatar(12, 1, 1.0f, 2.0f);
        lt.entityType = "Lt";
        CHECK(injected.save(12, lt), "injected save Lt");

        theseed::db::MySQLConnection locker(cfg.mysql);
        CHECK(locker.connect(), "locker connect");

        // save：tbl_Lt 被锁 → INSERT 等锁超时
        CHECK(locker.execute("LOCK TABLES `tbl_Lt` WRITE"), "lock tbl_Lt");
        CHECK(!injected.save(12, lt), "save blocked by table lock");
        CHECK(locker.execute("UNLOCK TABLES"), "unlock tbl_Lt");

        // load：tbl_Avatar 被锁 → SELECT 等锁超时
        CHECK(locker.execute("LOCK TABLES `tbl_Avatar` WRITE"), "lock tbl_Avatar");
        {
            EntityData loaded;
            CHECK(!injected.load(1, "Avatar", loaded), "load blocked by lock");
        }
        CHECK(locker.execute("UNLOCK TABLES"), "unlock tbl_Avatar");

        // remove：tbl_Avatar 被锁 → DELETE 等锁超时
        CHECK(locker.execute("LOCK TABLES `tbl_Avatar` WRITE"), "lock tbl_Avatar again");
        CHECK(!injected.remove(1), "remove blocked by lock");
        CHECK(locker.execute("UNLOCK TABLES"), "unlock tbl_Avatar again");

        // allocId：_entity_ids 被锁 → upsert 等锁超时
        CHECK(locker.execute("LOCK TABLES `_entity_ids` WRITE"), "lock _entity_ids");
        CHECK(injected.allocId() == 0, "allocId blocked by lock");
        CHECK(locker.execute("UNLOCK TABLES"), "unlock _entity_ids");

        // createAccount：_account_index 被锁 → 查重失败视为不存在 →
        // allocId 与主表写入成功 → 索引 INSERT 等锁超时
        CHECK(locker.execute("LOCK TABLES `_account_index` WRITE"), "lock _account_index");
        {
            theseed::core::EntityId blockedId = 0;
            CHECK(!injected.createAccount("locked_out", "pw", blockedId),
                  "createAccount blocked by lock");
        }
        CHECK(locker.execute("UNLOCK TABLES"), "unlock _account_index");

        injected.executeRaw("DROP TABLE IF EXISTS `tbl_Lt`");
    }

    std::cout << "\nMySQLEntityStoreTest: all passed\n";
    return 0;
}
