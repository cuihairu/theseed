// PostgreSQLConnection 直连测试：在真实 PostgreSQL 上驱动 happy path 与错误
// 分支（坏 SQL、连接失败、断线后的 PQreset 重连、NULL/bytea/u64 参数与结果
// 解码）。需要 THESEED_PG_HOST 等环境变量，未设置时整体跳过。
#include "theseed/db/PostgreSQLConnection.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using theseed::db::PostgreSQLConnection;
using theseed::db::PostgreSQLConnectionConfig;
using theseed::db::PostgreSQLResult;
using theseed::db::SqlParam;

namespace {

int gFailures = 0;

#define CHECK(cond, msg)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            std::cout << "  FAILED: " << msg << std::endl;   \
            ++gFailures;                                     \
        }                                                    \
    } while (0)

PostgreSQLConnectionConfig configFromEnv() {
    PostgreSQLConnectionConfig cfg;
    if (const char* h = std::getenv("THESEED_PG_HOST")) cfg.host = h;
    if (const char* p = std::getenv("THESEED_PG_PORT"))
        cfg.port = static_cast<std::uint16_t>(std::atoi(p));
    if (const char* u = std::getenv("THESEED_PG_USER")) cfg.user = u;
    if (const char* pw = std::getenv("THESEED_PG_PASSWORD")) cfg.password = pw;
    if (const char* db = std::getenv("THESEED_PG_DATABASE")) cfg.database = db;
    cfg.connectTimeoutSeconds = 3;
    return cfg;
}

}  // namespace

int main() {
    if (std::getenv("THESEED_PG_HOST") == nullptr) {
        std::cout << "PostgreSQLConnectionTest: skipped (THESEED_PG_HOST not set)" << std::endl;
        return 0;
    }

    const auto cfg = configFromEnv();

    // 默认构造的结果对象：所有访问器走防御分支；移动赋值可用
    {
        PostgreSQLResult empty;
        CHECK(!empty.next(), "empty result next");
        CHECK(empty.columnCount() == 0, "empty result columnCount");
        CHECK(empty.rowCount() == 0, "empty result rowCount");
        CHECK(empty.asBytes(0).empty(), "empty result asBytes");
        CHECK(empty.asString(0).empty(), "empty result asString");
        CHECK(empty.asUint64(0) == 0, "empty result asUint64");
        PostgreSQLResult moved;
        moved = std::move(empty);
        CHECK(!moved.next(), "move-assigned empty result");
    }

    // 连不上（错误端口）：connect 失败 + captureError + ensureConnected 兜底
    {
        auto bad = cfg;
        bad.port = 1;
        PostgreSQLConnection c(bad);
        CHECK(!c.connect(), "connect to dead port should fail");
        CHECK(!c.isConnected(), "not connected after failed connect");
        CHECK(!c.lastError().empty(), "lastError after failed connect");
        CHECK(!c.execute("SELECT 1"), "execute without connection should fail");
        CHECK(!c.query("SELECT 1").has_value(), "query without connection should fail");
        CHECK(!c.ping(), "ping without connection should fail");
        CHECK(!c.ensureConnected(), "ensureConnected to dead port should fail");
    }

    // 正常连接。高负载下 rootless podman 的用户态端口转发可能让握手超过
    // connect_timeout（环境抖动，非被测行为）：connect 失败后内部 conn 已
    // 复位为空，可原地有界重试。本文件多处直连共用同一重试策略。
    auto connectWithRetry = [](PostgreSQLConnection& conn) {
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
            if (attempt > 0) {
                std::cout << "  (connect retry " << attempt << " after: "
                          << conn.lastError() << ")" << std::endl;
            }
            ok = conn.connect();
        }
        return ok;
    };

    PostgreSQLConnection c(cfg);
    CHECK(connectWithRetry(c), "connect");
    CHECK(c.connect(), "second connect is idempotent");
    CHECK(c.isConnected(), "isConnected");

    // execute：DDL、坏 SQL、参数执行
    CHECK(c.execute("DROP TABLE IF EXISTS cov_t"), "drop temp table");
    CHECK(c.execute("CREATE TEMP TABLE cov_t ("
                    "id BIGSERIAL PRIMARY KEY,"
                    "v BYTEA)"),
          "create temp table");
    CHECK(!c.execute("THIS IS NOT SQL"), "bad SQL should fail execute");
    CHECK(!c.lastError().empty(), "lastError after bad SQL");
    CHECK(!c.execute("SELECT * FROM no_such_table_cov"), "execute on missing table should fail");

    // 带参数的 execute：bytea 走 \x 十六进制编码
    const std::vector<std::byte> payload{std::byte{0xCA}, std::byte{0xFE}};
    CHECK(c.execute("INSERT INTO cov_t (v) VALUES ($1::bytea)", {SqlParam(payload)}),
          "execute insert with bytea param");
    CHECK(c.affectedRows() == 1, "affectedRows after insert");

    // query：u64 / 字符串 / NULL / bytea 结果解码
    {
        auto r = c.query("SELECT $1::bigint AS num, $2::text AS txt, NULL AS nothing,"
                         " $3::bytea AS blob",
                         {SqlParam::u64(42), SqlParam::str("hello"), SqlParam(payload)});
        CHECK(r.has_value(), "query with params");
        if (r.has_value()) {
            CHECK(r->columnCount() == 4, "columnCount");
            CHECK(r->rowCount() == 1, "rowCount");
            CHECK(r->next(), "first next");
            CHECK(r->asUint64(0) == 42, "u64 roundtrip");
            CHECK(r->asString(1) == "hello", "string roundtrip");
            CHECK(r->asBytes(2).empty(), "NULL column yields empty bytes");
            CHECK(r->asBytes(3).size() == 2, "bytea decoded to original bytes");
            CHECK(r->asBytes(3)[0] == std::byte{0xCA}, "bytea first byte");
            CHECK(!r->next(), "next at end is false");
        }
    }

    // NULL 参数直接可读回 NULL
    {
        auto r = c.query("SELECT $1::text AS t", {SqlParam::null()});
        CHECK(r.has_value() && r->next() && r->asBytes(0).empty(), "NULL param roundtrip");
    }

    // query 错误分支：坏 SQL / 非 SELECT / 缺表
    CHECK(!c.query("THIS IS NOT SQL").has_value(), "query bad SQL returns nullopt");
    CHECK(!c.lastError().empty(), "lastError after bad query");
    CHECK(!c.query("SET @cov=1").has_value(), "non-SELECT query returns nullopt");
    CHECK(!c.query("SELECT * FROM no_such_table_cov").has_value(),
          "query on missing table returns nullopt");

    // ping 与断线重连：从第二条连接踢掉本连接的 backend，
    // 随后的命令触发 PQexecParams 失败，ensureConnected 走 PQreset 恢复
    CHECK(c.ping(), "ping");
    {
        PostgreSQLConnection killer(cfg);
        CHECK(connectWithRetry(killer), "killer connect");
        auto pid = c.query("SELECT pg_backend_pid()");
        CHECK(pid.has_value(), "read own pid");
        if (pid.has_value() && pid->next()) {
            const auto myPid = pid->asUint64(0);
            CHECK(killer.query("SELECT pg_terminate_backend($1::int)",
                               {SqlParam::u64(myPid)}).has_value(),
                  "terminate own backend");
        }
    }
    // libpq 只在下一条命令上才发现断线；此时 query/execute 走 res==nullptr
    // 分支，之后的 ensureConnected 用 PQreset 恢复
    CHECK(!c.query("SELECT 1").has_value(), "query on broken connection fails");
    CHECK(!c.lastError().empty(), "lastError on broken connection");
    CHECK(!c.execute("SELECT 1"), "execute on broken connection fails");
    CHECK(c.ensureConnected(), "ensureConnected recovers via PQreset");
    CHECK(c.ping(), "ping after recovery");
    auto recovered = c.query("SELECT 7 AS v");
    CHECK(recovered.has_value() && recovered->next() && recovered->asUint64(0) == 7,
          "query works after recovery");

    // disconnect 关闭连接
    c.disconnect();
    CHECK(!c.isConnected(), "not connected after disconnect");

    if (gFailures == 0) {
        std::cout << "PostgreSQLConnectionTest: all passed" << std::endl;
    } else {
        std::cout << "PostgreSQLConnectionTest: " << gFailures << " failure(s)" << std::endl;
    }
    return gFailures == 0 ? 0 : 1;
}
