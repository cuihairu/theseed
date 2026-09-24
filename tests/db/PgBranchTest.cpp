// PostgreSQL 后端分支覆盖测试（第四批）：存储层的未 init 防御族/空 blob/
// DROP 后缓存命中失败族/allocId 与账号连锁失败。与 PostgreSQLEntityStoreTest
// 互补——那边收功能回环，这边专收防御与失败分支。PG 的 ensureTable 失败臂
// 无法 SQL-only 注入（超长标识符被截断而非报错，与 MySQL 不同），不在此列。
// 需要 THESEED_PG_HOST 等环境变量，未设置时整体跳过。
#include "theseed/core/EntityData.h"
#include "theseed/db/PostgreSQLEntityStore.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

using theseed::core::DataType;
using theseed::core::EntityData;
using theseed::db::PostgreSQLEntityStore;

namespace {

int gFailures = 0;

#define CHECK(cond, msg)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            std::cout << "  FAILED: " << msg << std::endl;   \
            ++gFailures;                                     \
        }                                                    \
    } while (0)

PostgreSQLEntityStore::Config storeConfigFromEnv() {
    PostgreSQLEntityStore::Config cfg;
    auto& pg = cfg.pg;
    if (const char* h = std::getenv("THESEED_PG_HOST")) pg.host = h;
    if (const char* p = std::getenv("THESEED_PG_PORT"))
        pg.port = static_cast<std::uint16_t>(std::atoi(p));
    if (const char* u = std::getenv("THESEED_PG_USER")) pg.user = u;
    if (const char* pw = std::getenv("THESEED_PG_PASSWORD")) pg.password = pw;
    if (const char* db = std::getenv("THESEED_PG_DATABASE")) pg.database = db;
    cfg.autoCreateSchema = true;
    return cfg;
}

EntityData makeAvatar(theseed::core::EntityId id) {
    EntityData data;
    data.id = id;
    data.entityType = "Avatar";
    auto& prop = data.properties.emplace_back();
    prop.id = 0;
    prop.name = "level";
    prop.type = DataType::Int32;
    const std::int32_t level = 7;
    prop.rawValue.assign(reinterpret_cast<const std::byte*>(&level),
                         reinterpret_cast<const std::byte*>(&level) + sizeof(level));
    return data;
}

}  // namespace

int main() {
    if (std::getenv("THESEED_PG_HOST") == nullptr) {
        std::cout << "PgBranchTest: skipped (THESEED_PG_HOST not set)" << std::endl;
        return 0;
    }

    // 未 init 的 store：conn_ 为空，全部 API 走防御臂。
    {
        PostgreSQLEntityStore store(storeConfigFromEnv());  // 不调 init
        CHECK(!store.executeRaw("SELECT 1"), "uninit executeRaw");
        EntityData out;
        CHECK(!store.load(1, "Avatar", out), "uninit load");
        CHECK(!store.save(1, makeAvatar(1)), "uninit save");
        CHECK(!store.remove(1), "uninit remove");
        CHECK(store.allocId() == 0, "uninit allocId");
        CHECK(store.listIdsByType("Avatar").empty(), "uninit listIdsByType");
        CHECK(store.listEntityTypes().empty(), "uninit listEntityTypes");
        theseed::core::EntityId id = 0;
        std::string pw;
        CHECK(!store.queryAccount("u", id, pw), "uninit queryAccount");
        CHECK(!store.createAccount("u", "p", id), "uninit createAccount");
        CHECK(!store.lastError().empty(), "uninit lastError");
    }

    PostgreSQLEntityStore store(storeConfigFromEnv());
    CHECK(store.init(), "store init");
    static_cast<void>(store.executeRaw("DROP TABLE IF EXISTS \"tbl_Avatar\""));

    // 正常往返 + 空 blob + executeRaw 失败臂 + DROP 后缓存命中失败族。
    {
        CHECK(store.save(910001, makeAvatar(910001)), "save avatar");
        EntityData out;
        CHECK(store.load(910001, "Avatar", out), "load avatar back");

        // data 列为空串：load 走 bytes.empty() 失败臂。
        CHECK(store.executeRaw(
                  "INSERT INTO \"tbl_Avatar\" (id, data) VALUES (910002, '')"),
              "insert empty blob");
        CHECK(!store.load(910002, "Avatar", out), "load rejects empty blob");

        CHECK(!store.executeRaw("THIS IS NOT SQL"), "executeRaw rejects bad SQL");

        CHECK(store.executeRaw("DROP TABLE \"tbl_Avatar\""), "drop table under cache");
        CHECK(!store.load(910001, "Avatar", out), "load fails after drop");
        CHECK(!store.save(910001, makeAvatar(910001)), "save fails after drop");
        CHECK(store.listIdsByType("Avatar").empty(), "listIdsByType fails after drop");
        CHECK(!store.remove(910001), "remove fails after drop");
        static_cast<void>(store.executeRaw("DROP TABLE IF EXISTS \"tbl_Avatar\""));
    }

    // sanitize 全谱系字符：AZ[az{09:_- 覆盖 sanitizeForTable 每个比较段的
    // 高低两向（大写、'Z' 之后 '['，小写、'z' 之后 '{'，数字、'9' 之后 ':'，
    // 合法 '_'，非法 '-'）。表名合法化后正常建表（PG 的 DDL 失败臂造不出，
    // 但 sanitize 分支与 DDL 结果无关），收尾 DROP。
    {
        const std::string spectrum = "AZ[az{09:_-";
        CHECK(store.listIdsByType(spectrum).empty(),
              "listIdsByType on sanitized spectrum type");
        EntityData out;
        CHECK(!store.load(1, spectrum, out), "load empty spectrum table");
        auto data = makeAvatar(1);
        data.entityType = spectrum;
        CHECK(store.save(1, data), "save spectrum type");
        CHECK(store.load(1, spectrum, out), "load spectrum back");
        CHECK(store.executeRaw("DROP TABLE \"tbl_AZ_az_09___\""), "drop spectrum table");
    }

    // allocId 失败连锁：DROP _entity_ids → INSERT..RETURNING 失败 →
    // allocId 0 → createAccount 失败。
    {
        CHECK(store.executeRaw("DROP TABLE _entity_ids"), "drop id table");
        CHECK(store.allocId() == 0, "allocId fails without id table");
        theseed::core::EntityId id = 0;
        CHECK(!store.createAccount("covbr_u1", "pw", id), "createAccount fails on allocId 0");
    }

    // 新 store init：createSchema 重建 _entity_ids / _account_index。
    {
        PostgreSQLEntityStore rebuild(storeConfigFromEnv());
        CHECK(rebuild.init(), "rebuild schema");
    }

    // _account_index 缺失连锁：queryAccount 直接失败；createAccount 走完
    // dup-null 短路与主表写入，最终在索引插入处失败。
    {
        CHECK(store.executeRaw("DROP TABLE _account_index"), "drop account index");
        theseed::core::EntityId id = 0;
        std::string pw;
        CHECK(!store.queryAccount("covbr_u2", id, pw), "queryAccount fails without index");
        CHECK(!store.createAccount("covbr_u2", "pw", id),
              "createAccount fails on index insert");
    }

    // tbl_account 缺失：createAccount 在主表 save 处失败。
    {
        static_cast<void>(store.executeRaw("DROP TABLE IF EXISTS \"tbl_Account\""));
        theseed::core::EntityId id = 0;
        CHECK(!store.createAccount("covbr_u3", "pw", id),
              "createAccount fails on entity save");
    }

    // 幸福路径回归：新实例（knownTables_ 为空，ensureTable 会重建 B7 删掉的
    // 主表；init 重建 B6 删掉的索引表），确认防御场景没污染正常语义。
    {
        PostgreSQLEntityStore fresh(storeConfigFromEnv());
        CHECK(fresh.init(), "fresh store init");
        theseed::core::EntityId id = 0;
        CHECK(fresh.createAccount("covbr_u4", "pw4", id), "createAccount happy path");
        CHECK(id != 0, "created id nonzero");
        std::string pw;
        CHECK(fresh.queryAccount("covbr_u4", id, pw), "queryAccount roundtrip");
        CHECK(pw == "pw4", "password roundtrip");
    }

    // 收尾：重建系统表，不留缺失表状态给后续测试。
    {
        PostgreSQLEntityStore cleanup(storeConfigFromEnv());
        CHECK(cleanup.init(), "cleanup init recreates schema");
    }

    if (gFailures == 0) {
        std::cout << "PgBranchTest: all passed" << std::endl;
    } else {
        std::cout << "PgBranchTest: " << gFailures << " failure(s)" << std::endl;
    }
    return gFailures == 0 ? 0 : 1;
}
