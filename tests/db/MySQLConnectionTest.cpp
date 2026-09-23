// MySQLConnection 直连测试：在真实 MySQL 上驱动 happy path 与错误分支
// （坏 SQL、参数个数不匹配、非 SELECT 的 query、多语句排空、NULL 列、
// 超过单列缓冲的截断恢复、重连失败）。需要 THESEED_MYSQL_HOST 等环境变量，
// 未设置时整体跳过。
#include "theseed/db/MySQLConnection.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using theseed::db::MySQLConnection;
using theseed::db::MySQLConnectionConfig;
using theseed::db::MySQLResult;
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

MySQLConnectionConfig configFromEnv() {
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

}  // namespace

int main() {
    if (std::getenv("THESEED_MYSQL_HOST") == nullptr) {
        std::cout << "MySQLConnectionTest: skipped (THESEED_MYSQL_HOST not set)" << std::endl;
        return 0;
    }

    const auto cfg = configFromEnv();

    // 空结果对象：next/asBytes/columnCount/rowCount 全走防御分支；移动语义可用
    {
        MySQLResult empty;
        CHECK(!empty.next(), "empty result next should be false");
        CHECK(empty.columnCount() == 0, "empty result columnCount");
        CHECK(empty.rowCount() == 0, "empty result rowCount");
        CHECK(empty.asBytes(0).empty(), "empty result asBytes");
        CHECK(empty.asUint64(0) == 0, "empty result asUint64");
        CHECK(empty.asString(0).empty(), "empty result asString");
        MySQLResult moved = std::move(empty);
        CHECK(!moved.next(), "moved result next");
    }

    // 连不上（错误端口）：connect 失败 + captureError + autoReconnect=false 直返
    {
        auto bad = cfg;
        bad.port = 1;
        bad.autoReconnect = false;
        MySQLConnection c(bad);
        CHECK(!c.connect(), "connect to dead port should fail");
        CHECK(!c.isConnected(), "not connected after failed connect");
        CHECK(!c.lastError().empty(), "lastError captured after failed connect");
        CHECK(!c.execute("SELECT 1"), "execute without connection should fail");
        CHECK(!c.query("SELECT 1").has_value(), "query without connection should fail");
        CHECK(!c.executeParams("SELECT ?", {MySqlParam::u64(1)}),
              "executeParams without connection should fail");
        CHECK(!c.ping(), "ping without connection should fail");
    }

    // autoReconnect=true：ensureConnected 触发重连尝试，仍然失败
    {
        auto bad = cfg;
        bad.port = 1;
        bad.autoReconnect = true;
        MySQLConnection c(bad);
        CHECK(!c.execute("SELECT 1"), "autoReconnect to dead port should fail");
        CHECK(!c.lastError().empty(), "lastError after failed reconnect");
    }

    // 正常连接
    MySQLConnection c(cfg);
    CHECK(c.connect(), "connect");
    CHECK(c.connect(), "second connect is idempotent");
    CHECK(c.isConnected(), "isConnected");
    CHECK(c.ping(), "ping");

    // disconnect 关闭连接并复位句柄；重连后依然可用
    c.disconnect();
    CHECK(!c.isConnected(), "not connected after disconnect");
    CHECK(!c.ping(), "ping after disconnect");
    CHECK(c.connect(), "reconnect after disconnect");
    CHECK(c.ping(), "ping after reconnect");

    // move 赋值
    {
        MySQLResult src;
        MySQLResult dst;
        dst = std::move(src);
        CHECK(!dst.next(), "move-assigned empty result");
    }

    // execute 错误与多语句排空
    CHECK(!c.execute("THIS IS NOT SQL"), "bad SQL should fail execute");
    CHECK(c.execute("DROP TEMPORARY TABLE IF EXISTS cov_t"), "create temp table prep");
    CHECK(c.execute("CREATE TEMPORARY TABLE cov_t ("
                    "id BIGINT AUTO_INCREMENT PRIMARY KEY,"
                    "v LONGBLOB)"),
          "create temp table");
    CHECK(c.execute("SELECT 1; SELECT 2;"), "multi statement execute");
    CHECK(!c.query("SET @cov=1").has_value(), "query on non-SELECT returns nullopt");
    CHECK(!c.query("SELECT * FROM no_such_table_cov").has_value(),
          "query on missing table returns nullopt");
    CHECK(!c.lastError().empty(), "lastError after failed query");

    // executeParams 错误分支：prepare 失败 / 参数个数不匹配 / execute 失败
    CHECK(!c.executeParams("NOT VALID SQL ?", {}),
          "executeParams prepare failure");
    CHECK(!c.executeParams("SELECT ?", {}),
          "executeParams param count mismatch");
    CHECK(!c.executeParams("INSERT INTO no_such_table_cov VALUES (?)",
                           {MySqlParam::u64(1)}),
          "executeParams execute failure");

    // executeParams 成功路径 + lastInsertId/affectedRows
    const std::vector<std::byte> payload{std::byte{0xCA}, std::byte{0xFE}};
    CHECK(c.executeParams("INSERT INTO cov_t (v) VALUES (?)", {MySqlParam(payload)}),
          "executeParams insert");
    CHECK(c.affectedRows() == 1, "affectedRows after insert");
    CHECK(c.lastInsertId() != 0, "lastInsertId after insert");

    // queryParams 错误分支：prepare 失败 / 个数不匹配 / execute 失败 / 非 SELECT
    CHECK(!c.queryParams("NOT VALID SQL ?", {}).has_value(),
          "queryParams prepare failure");
    CHECK(!c.queryParams("SELECT ?,?", {MySqlParam::u64(1)}).has_value(),
          "queryParams param count mismatch");
    CHECK(!c.queryParams("SELECT * FROM no_such_table_cov WHERE id=?",
                         {MySqlParam::u64(1)}).has_value(),
          "queryParams execute failure");
    CHECK(!c.queryParams("INSERT INTO cov_t (v) VALUES (?)", {MySqlParam(payload)}).has_value(),
          "queryParams on non-SELECT returns nullopt");

    // queryParams 成功路径：u64 绑定、字符串绑定、NULL 列、70KB 截断恢复、
    // materialized 结果的 next()/advance() 全程
    {
        auto r = c.queryParams("SELECT ? AS num, ? AS txt, NULL AS nothing,"
                               " REPEAT('x', 70000) AS big",
                               {MySqlParam::u64(42), MySqlParam::str("hello")});
        CHECK(r.has_value(), "queryParams select");
        if (r.has_value()) {
            CHECK(r->columnCount() == 4, "columnCount");
            CHECK(r->rowCount() == 1, "rowCount");
            CHECK(r->next(), "first next");
            CHECK(r->asUint64(0) == 42, "u64 roundtrip");
            CHECK(r->asString(1) == "hello", "string roundtrip");
            CHECK(r->asBytes(2).empty(), "NULL column yields empty bytes");
            CHECK(r->asBytes(3).size() == 70000, "truncated column recovered");
            CHECK(r->asString(3).substr(69990) == "xxxxxxxxxx",
                  "truncated column tail intact");
            CHECK(!r->next(), "next at end is false");
            CHECK(!r->next(), "next past end is false");
        }
    }

    // 参数超过 max_allowed_packet（容器默认 64MB）：prepare 成功但 execute 失败，
    // 覆盖 executeParams/queryParams 的 execute 错误分支
    {
        const std::vector<std::byte> huge(70u * 1024u * 1024u, std::byte{'x'});
        CHECK(!c.executeParams("INSERT INTO cov_t (v) VALUES (?)", {MySqlParam(huge)}),
              "executeParams oversized packet should fail");
        CHECK(!c.lastError().empty(), "lastError after oversized executeParams");
        CHECK(!c.queryParams("SELECT ?", {MySqlParam(huge)}).has_value(),
              "queryParams oversized packet should fail");
        CHECK(!c.lastError().empty(), "lastError after oversized queryParams");
    }

    if (gFailures == 0) {
        std::cout << "MySQLConnectionTest: all passed" << std::endl;
    } else {
        std::cout << "MySQLConnectionTest: " << gFailures << " failure(s)" << std::endl;
    }
    return gFailures == 0 ? 0 : 1;
}
