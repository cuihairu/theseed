// MySQL 后端分支覆盖测试（第四批）：连接层的 charset 缺省/二次 disconnect/
// 结果集防御臂/空参数列表/掉线重连语义，存储层的未 init 防御族/非法表名
// DDL 失败/DROP 后缓存命中失败族/allocId 与账号连锁失败。与
// MySQLConnectionTest / MySqlEntityStoreTest 互补——那边收功能回环，这边专收
// 防御与失败分支。需要 THESEED_MYSQL_HOST 等环境变量，未设置时整体跳过。
#include "theseed/core/EntityData.h"
#include "theseed/db/MySQLConnection.h"
#include "theseed/db/MySQLEntityStore.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using theseed::core::DataType;
using theseed::core::EntityData;
using theseed::db::MySQLConnection;
using theseed::db::MySQLConnectionConfig;
using theseed::db::MySQLEntityStore;
using theseed::db::MySqlParam;

namespace {

int gFailures = 0;

#define CHECK(cond, msg)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            std::cout << "  FAILED: " << msg << std::endl;   \
            ++gFailures;                                     \
        }                                                    \
    } while (0)

MySQLConnectionConfig connConfigFromEnv() {
    MySQLConnectionConfig cfg;
    if (const char* h = std::getenv("THESEED_MYSQL_HOST")) cfg.host = h;
    if (const char* p = std::getenv("THESEED_MYSQL_PORT"))
        cfg.port = static_cast<std::uint16_t>(std::atoi(p));
    if (const char* u = std::getenv("THESEED_MYSQL_USER")) cfg.user = u;
    if (const char* pw = std::getenv("THESEED_MYSQL_PASSWORD")) cfg.password = pw;
    if (const char* db = std::getenv("THESEED_MYSQL_DATABASE")) cfg.database = db;
    cfg.connectTimeoutSeconds = 3;
    return cfg;
}

MySQLEntityStore::Config storeConfigFromEnv() {
    MySQLEntityStore::Config cfg;
    cfg.mysql = connConfigFromEnv();
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
    if (std::getenv("THESEED_MYSQL_HOST") == nullptr) {
        std::cout << "MySqlBranchTest: skipped (THESEED_MYSQL_HOST not set)" << std::endl;
        return 0;
    }

    // ---- 连接层 ----

    // charset 留空：跳过 SET NAMES 直接连（177 的 false 臂）。
    {
        auto cfg = connConfigFromEnv();
        cfg.charset.clear();
        MySQLConnection c(cfg);
        CHECK(c.connect(), "connect with empty charset");
        CHECK(c.ping(), "ping after empty-charset connect");
    }

    // 二次 disconnect：connected 已复位的防御臂（204 的 false 臂）。
    {
        MySQLConnection c(connConfigFromEnv());
        CHECK(c.connect(), "connect for double disconnect");
        c.disconnect();
        c.disconnect();
        CHECK(!c.isConnected(), "still disconnected after double disconnect");
    }

    // asUint64 遇非数字文本：bytesToUint64 的非数字 break 臂（26）。
    // 'not-a-number' 首字符 n > '9'（高向 break），'-42' 首字符 - < '0'（低向）。
    {
        MySQLConnection c(connConfigFromEnv());
        CHECK(c.connect(), "connect for non-numeric asUint64");
        auto r = c.query("SELECT 'not-a-number' AS v");
        CHECK(r.has_value(), "query string literal");
        if (r.has_value()) {
            CHECK(r->next(), "next on string literal row");
            CHECK(r->asUint64(0) == 0, "non-numeric column yields 0");
        }
        auto neg = c.query("SELECT '-42' AS v");
        CHECK(neg.has_value(), "query negative literal");
        if (neg.has_value()) {
            CHECK(neg->next(), "next on negative literal row");
            CHECK(neg->asUint64(0) == 0, "leading low char yields 0");
        }
    }

    // asBytes 防御臂：原生结果不 next 就读 / 列越界；materialized 结果同型。
    {
        MySQLConnection c(connConfigFromEnv());
        CHECK(c.connect(), "connect for asBytes guards");
        auto native = c.query("SELECT 41 AS a");
        CHECK(native.has_value(), "native query");
        if (native.has_value()) {
            CHECK(native->asBytes(0).empty(), "asBytes before next on native result");
            CHECK(native->next(), "next on native result");
            CHECK(native->asBytes(99).empty(), "asBytes out-of-range col on native result");
        }
        auto mat = c.queryParams("SELECT ? AS a", {MySqlParam::u64(42)});
        CHECK(mat.has_value(), "materialized query");
        if (mat.has_value()) {
            CHECK(mat->asBytes(0).empty(), "asBytes before next on materialized result");
            CHECK(mat->next(), "next on materialized result");
            CHECK(mat->asBytes(99).empty(), "asBytes out-of-range col on materialized result");
            CHECK(mat->asUint64(0) == 42, "value intact after guard probes");
        }
    }

    // 多语句排空：三条语句全部执行并排空残留结果集。
    {
        MySQLConnection c(connConfigFromEnv());
        CHECK(c.connect(), "connect for multi statement");
        CHECK(c.execute("SELECT 1; SELECT 2; SELECT 3;"), "multi statement drain");
        CHECK(c.execute("SELECT 1"), "connection still usable after drain");
    }

    // 空参数列表：绑定循环零次、queryParams 走无参路径（361 的 false 臂）。
    {
        MySQLConnection c(connConfigFromEnv());
        CHECK(c.connect(), "connect for empty params");
        CHECK(c.executeParams("SET @cov_br = 1", {}), "executeParams with empty list");
        auto r = c.queryParams("SELECT @cov_br + 40 AS v", {});
        CHECK(r.has_value(), "queryParams with empty list");
        if (r.has_value()) {
            CHECK(r->next(), "next on empty-param query");
            CHECK(r->asUint64(0) == 41, "session var roundtrip");
        }
    }

    // 掉线重连语义：wait_timeout=1 让服务端 2 秒内主动断开空闲连接。
    {
        // autoReconnect=true：mysql_ping 在 ensureConnected 内透明重连，请求成功。
        auto cfg = connConfigFromEnv();
        MySQLConnection c(cfg);
        CHECK(c.connect(), "connect for auto reconnect probe");
        CHECK(c.execute("SET SESSION wait_timeout = 1"), "set wait_timeout");
        std::this_thread::sleep_for(std::chrono::seconds(2));
        CHECK(c.execute("SELECT 1"), "ping-driven transparent reconnect");

        // autoReconnect=false：ping 失败臂（535 的 branch6）命中，execute 报失败。
        auto noRe = connConfigFromEnv();
        noRe.autoReconnect = false;
        MySQLConnection d(noRe);
        CHECK(d.connect(), "connect without auto reconnect");
        CHECK(d.execute("SET SESSION wait_timeout = 1"), "set wait_timeout (no-reconnect)");
        std::this_thread::sleep_for(std::chrono::seconds(2));
        CHECK(!d.execute("SELECT 1"), "dead connection fails without reconnect");
    }

    // ---- 存储层 ----

    // 未 init 的 store：conn_ 为空，全部 API 走防御臂。
    {
        MySQLEntityStore store(storeConfigFromEnv());  // 不调 init
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

    MySQLEntityStore store(storeConfigFromEnv());
    CHECK(store.init(), "store init");
    static_cast<void>(store.executeRaw("DROP TABLE IF EXISTS `tbl_Avatar`"));

    // 非法表名：entityType 混入非法字符且超长（tbl_ + 80 字符 > MySQL 64 上限）
    // → ensureTable 的 DDL 失败臂，连带 load/save/listIdsByType 失败。
    {
        const std::string longType = "A-2_xA-2_xA-2_xA-2_xA-2_xA-2_xA-2_xA-2_xA-2_xA-2_x"
                                     "A-2_xA-2_xA-2_xA-2_xA-2_xA-2_x";
        EntityData out;
        CHECK(!store.load(1, longType, out), "load with overlong table name");
        CHECK(!store.lastError().empty(), "lastError after DDL failure");
        CHECK(store.listIdsByType(longType).empty(), "listIdsByType with overlong table name");
        auto data = makeAvatar(1);
        data.entityType = longType;
        CHECK(!store.save(1, data), "save with overlong table name");
    }

    // sanitize 全谱系字符：AZ[az{09:_- 覆盖每个比较段的高低两向（大写、'Z'
    // 之后 '['，小写、'z' 之后 '{'，数字、'9' 之后 ':'，合法 '_'，非法 '-'），
    // 驱满 sanitizeForTable 的全部字符类别边。表名合法不超长，ensureTable
    // 正常建表，收尾 DROP。
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
        CHECK(store.executeRaw("DROP TABLE `tbl_AZ_az_09___`"), "drop spectrum table");
    }

    // 正常往返 + 空 blob + DROP 后缓存命中失败族：ensureTable 已缓存表名，
    // DROP 后 DELETE/SELECT/INSERT 直接命中 SQL 错误臂。
    {
        CHECK(store.save(910001, makeAvatar(910001)), "save avatar");
        EntityData out;
        CHECK(store.load(910001, "Avatar", out), "load avatar back");

        // data 列为空串：load 走 bytes.empty() 失败臂。
        CHECK(store.executeRaw(
                  "INSERT INTO `tbl_Avatar` (`id`, `data`) VALUES (910002, '')"),
              "insert empty blob");
        CHECK(!store.load(910002, "Avatar", out), "load rejects empty blob");

        // executeRaw 自身的失败臂。
        CHECK(!store.executeRaw("THIS IS NOT SQL"), "executeRaw rejects bad SQL");

        CHECK(store.executeRaw("DROP TABLE `tbl_Avatar`"), "drop table under cache");
        CHECK(!store.load(910001, "Avatar", out), "load fails after drop");
        CHECK(!store.save(910001, makeAvatar(910001)), "save fails after drop");
        CHECK(store.listIdsByType("Avatar").empty(), "listIdsByType fails after drop");
        CHECK(!store.remove(910001), "remove fails after drop");
        static_cast<void>(store.executeRaw("DROP TABLE IF EXISTS `tbl_Avatar`"));
    }

    // allocId 失败连锁：DROP _entity_ids → allocId 0 → createAccount 失败。
    {
        CHECK(store.executeRaw("DROP TABLE `_entity_ids`"), "drop id table");
        CHECK(store.allocId() == 0, "allocId fails without id table");
        theseed::core::EntityId id = 0;
        CHECK(!store.createAccount("covbr_u1", "pw", id), "createAccount fails on allocId 0");
    }

    // 新 store init：createSchema 重建 _entity_ids / _account_index。
    {
        MySQLEntityStore rebuild(storeConfigFromEnv());
        CHECK(rebuild.init(), "rebuild schema");
    }

    // _account_index 缺失连锁：queryAccount 直接失败；createAccount 走完
    // dup-null 短路与主表写入，最终在索引插入处失败。
    {
        CHECK(store.executeRaw("DROP TABLE `_account_index`"), "drop account index");
        theseed::core::EntityId id = 0;
        std::string pw;
        CHECK(!store.queryAccount("covbr_u2", id, pw), "queryAccount fails without index");
        CHECK(!store.createAccount("covbr_u2", "pw", id),
              "createAccount fails on index insert");
    }

    // tbl_account 缺失：createAccount 在主表 save 处失败。
    {
        static_cast<void>(store.executeRaw("DROP TABLE IF EXISTS `tbl_Account`"));
        theseed::core::EntityId id = 0;
        CHECK(!store.createAccount("covbr_u3", "pw", id),
              "createAccount fails on entity save");
    }

    // 幸福路径回归：新实例（knownTables_ 为空，ensureTable 会重建 B7 删掉的
    // 主表；init 重建 B6 删掉的索引表），确认防御场景没污染正常语义。
    {
        MySQLEntityStore fresh(storeConfigFromEnv());
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
        MySQLEntityStore cleanup(storeConfigFromEnv());
        CHECK(cleanup.init(), "cleanup init recreates schema");
    }

    if (gFailures == 0) {
        std::cout << "MySqlBranchTest: all passed" << std::endl;
    } else {
        std::cout << "MySqlBranchTest: " << gFailures << " failure(s)" << std::endl;
    }
    return gFailures == 0 ? 0 : 1;
}
