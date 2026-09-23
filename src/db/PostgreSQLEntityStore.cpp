#include "theseed/db/PostgreSQLEntityStore.h"
#include "theseed/foundation/Metrics.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <sstream>
#include <utility>

namespace theseed::db {

namespace {

// RAII tick 延迟计量器，与 MySQLEntityStore 中的实现一致。
class ScopedMsTimer final {
public:
    using Emitter = std::function<void(double)>;
    explicit ScopedMsTimer(Emitter emitter)
        : start_(std::chrono::steady_clock::now()), emitter_(std::move(emitter)) {}
    ~ScopedMsTimer() {
        if (emitter_) {
            emitter_(std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - start_).count());
        }
    }
    ScopedMsTimer(const ScopedMsTimer&) = delete;
    ScopedMsTimer& operator=(const ScopedMsTimer&) = delete;

private:
    std::chrono::steady_clock::time_point start_;
    Emitter emitter_;
};

foundation::Histogram& pgHistogram(const char* name, const char* desc) {
    return foundation::MetricsRegistry::instance().histogram(
        name,
        foundation::Histogram::Boundaries{0.5, 1.0, 2.0, 5.0, 10.0, 25.0, 50.0,
                                          100.0, 250.0, 500.0, 1000.0},
        desc);
}

std::string sanitizeForTable(const std::string& entityType) {
    std::string out;
    out.reserve(entityType.size());
    for (char c : entityType) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    return out;
}

std::vector<std::byte> strToBytes(const std::string& s) {
    return std::vector<std::byte>(
        reinterpret_cast<const std::byte*>(s.data()),
        reinterpret_cast<const std::byte*>(s.data()) + s.size());
}

}  // namespace

// ---------------------------------------------------------------------------
// 构造与初始化
// ---------------------------------------------------------------------------

PostgreSQLEntityStore::PostgreSQLEntityStore(Config config)
    : config_(std::move(config)) {}

PostgreSQLEntityStore::~PostgreSQLEntityStore() = default;  // LCOV_EXCL_LINE trivial 析构的 out-of-line 定义无机器码，gcc 不产生计数条目

bool PostgreSQLEntityStore::init() {
    if (config_.connection) {
        conn_ = config_.connection;
    } else {
        conn_ = std::make_shared<PostgreSQLConnection>(config_.pg);
        if (!conn_->connect()) {
            lastError_ = "PostgreSQL connect failed: " + conn_->lastError();
            return false;
        }
    }
    if (config_.autoCreateSchema) {
        if (!createSchema()) {
            return false;
        }
    }
    return true;
}

bool PostgreSQLEntityStore::createSchema() {
    // 全局 ID 分配表。不预置任何行——allocId() 的
    // ON CONFLICT DO UPDATE ... RETURNING 会按需创建 '__global__' 行。
    if (!conn_->execute(
            "CREATE TABLE IF NOT EXISTS _entity_ids ("
            "  entity_type VARCHAR(64) NOT NULL PRIMARY KEY,"
            "  next_id BIGINT NOT NULL DEFAULT 1"
            ")")) {
        lastError_ = "create _entity_ids failed: " + conn_->lastError();
        return false;
    }

    // Account 索引表。password 存哈希，BYTEA 与 MySQL 版 VARBINARY 语义一致。
    if (!conn_->execute(
            "CREATE TABLE IF NOT EXISTS _account_index ("
            "  username VARCHAR(128) NOT NULL PRIMARY KEY,"
            "  entity_id BIGINT NOT NULL,"
            "  password BYTEA NOT NULL"
            ")")) {
        // LCOV_EXCL_START PG16：schema ACL 检查先于 if_not_exists 存在性跳过、readonly 检查先于 analyze，SQL-only 无法构造第 1 条成功第 2 条失败（三重实验封死，见 docs/design/8-reference/coverage-report.md §6）
        lastError_ = "create _account_index failed: " + conn_->lastError();
        return false;
        // LCOV_EXCL_STOP
    }
    if (!conn_->execute(
            "CREATE INDEX IF NOT EXISTS idx_account_entity_id "
            "ON _account_index (entity_id)")) {
        lastError_ = "create _account_index index failed: " + conn_->lastError();
        return false;
    }
    return true;
}

std::string PostgreSQLEntityStore::tableName(const std::string& entityType) {
    return "tbl_" + sanitizeForTable(entityType);
}

bool PostgreSQLEntityStore::ensureTable(const std::string& entityType) {
    auto tbl = tableName(entityType);
    if (knownTables_.count(tbl) > 0) return true;

    // PG 索引名是 schema 级唯一的，按表名后缀命名避免冲突。
    std::ostringstream ddl;
    ddl << "CREATE TABLE IF NOT EXISTS \"" << tbl << "\" ("
        << "  id BIGINT NOT NULL PRIMARY KEY,"
        << "  data BYTEA NOT NULL,"
        << "  updated_at TIMESTAMPTZ NOT NULL DEFAULT now()"
        << ")";
    if (!conn_->execute(ddl.str())) {
        lastError_ = "ensureTable failed: " + conn_->lastError();
        return false;
    }
    std::ostringstream idx;
    idx << "CREATE INDEX IF NOT EXISTS \"idx_updated_" << tbl
        << "\" ON \"" << tbl << "\" (updated_at)";
    if (!conn_->execute(idx.str())) {
        lastError_ = "ensureTable index failed: " + conn_->lastError();
        return false;
    }
    knownTables_.insert(tbl);
    return true;
}

bool PostgreSQLEntityStore::ensureConnected() {
    if (!conn_) {
        lastError_ = "store not initialized";
        return false;
    }
    return conn_->ensureConnected();
}

const std::string& PostgreSQLEntityStore::lastError() const {
    return lastError_;
}

bool PostgreSQLEntityStore::executeRaw(const std::string& sql) {
    if (!ensureConnected()) return false;
    if (!conn_->execute(sql)) {
        lastError_ = "executeRaw failed: " + conn_->lastError();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// IEntityStore
// ---------------------------------------------------------------------------

bool PostgreSQLEntityStore::load(core::EntityId id, const std::string& entityType,
                                 core::EntityData& out) {
    ScopedMsTimer timer([](double ms) {
        pgHistogram("pg_load_ms", "PostgreSQL entity load latency").observe(ms);
    });
    if (!ensureConnected() || !ensureTable(entityType)) return false;

    auto tbl = tableName(entityType);
    std::ostringstream sql;
    sql << "SELECT \"data\" FROM \"" << tbl << "\" WHERE \"id\" = $1::bigint";
    auto result = conn_->query(sql.str(), {SqlParam::u64(id)});
    if (!result) {
        lastError_ = "load query failed: " + conn_->lastError();
        return false;
    }
    if (!result->next()) return false;  // 不存在
    auto bytes = result->asBytes(0);
    if (bytes.empty()) return false;

    foundation::MemoryStream ms;
    ms.writeBytes(bytes.data(), bytes.size());
    ms.resetRead();
    return core::decodeEntityData(ms, out);
}

bool PostgreSQLEntityStore::save(core::EntityId id, const core::EntityData& data) {
    ScopedMsTimer timer([](double ms) {
        pgHistogram("pg_save_ms", "PostgreSQL entity save latency").observe(ms);
    });
    if (!ensureConnected() || !ensureTable(data.entityType)) return false;

    // 序列化 EntityData 为二进制 BYTEA（与 MySQL/File 后端同格式）
    foundation::MemoryStream ms;
    core::encodeEntityData(ms, data);
    std::vector<std::byte> blob(ms.data(), ms.data() + ms.size());

    auto tbl = tableName(data.entityType);
    std::ostringstream sql;
    sql << "INSERT INTO \"" << tbl << "\" (\"id\", \"data\") VALUES ($1::bigint, $2::bytea) "
        << "ON CONFLICT (\"id\") DO UPDATE SET \"data\" = EXCLUDED.\"data\"";

    if (!conn_->execute(sql.str(), {SqlParam::u64(id), std::move(blob)})) {
        lastError_ = "save failed: " + conn_->lastError();
        return false;
    }
    return true;
}

bool PostgreSQLEntityStore::remove(core::EntityId id) {
    ScopedMsTimer timer([](double ms) {
        pgHistogram("pg_remove_ms", "PostgreSQL entity remove latency").observe(ms);
    });
    if (!ensureConnected()) return false;

    // Account 索引表清理（若该 id 是账号）
    conn_->execute("DELETE FROM _account_index WHERE entity_id = $1::bigint",
                   {SqlParam::u64(id)});

    // 扫描所有已知表删除（与 MySQLEntityStore 一致）
    auto types = listEntityTypes();
    bool removed = false;
    for (const auto& entityType : types) {
        ensureTable(entityType);
        auto tbl = tableName(entityType);
        std::ostringstream sql;
        sql << "DELETE FROM \"" << tbl << "\" WHERE \"id\" = $1::bigint";
        if (!conn_->execute(sql.str(), {SqlParam::u64(id)})) {
            lastError_ = "remove failed: " + conn_->lastError();
            return false;
        }
        if (conn_->affectedRows() > 0) removed = true;
    }
    return removed;
}

core::EntityId PostgreSQLEntityStore::allocId() {
    ScopedMsTimer timer([](double ms) {
        pgHistogram("pg_alloc_id_ms", "PostgreSQL allocId latency").observe(ms);
    });
    if (!ensureConnected()) return 0;

    // 全局原子自增计数器，单语句完成分配与取回：
    //   全新库走 INSERT 分支 → next_id=1，RETURNING 1（首个 id 为 1）
    //   已有行走 UPDATE 分支 → next_id = 旧值 + 1，RETURNING 新值
    // 真机验证过：连续调用依次返回 1、2、3。
    auto result = conn_->query(
        "INSERT INTO _entity_ids (entity_type, next_id) VALUES ('__global__', 1) "
        "ON CONFLICT (entity_type) DO UPDATE SET next_id = _entity_ids.next_id + 1 "
        "RETURNING next_id");
    if (!result || !result->next()) {
        lastError_ = "allocId failed: " + conn_->lastError();
        return 0;
    }
    return result->asUint64(0);
}

std::vector<core::EntityId> PostgreSQLEntityStore::listIdsByType(const std::string& entityType) {
    std::vector<core::EntityId> ids;
    if (!ensureConnected() || !ensureTable(entityType)) return ids;

    auto tbl = tableName(entityType);
    std::ostringstream sql;
    sql << "SELECT \"id\" FROM \"" << tbl << "\" ORDER BY \"id\"";
    auto result = conn_->query(sql.str());
    if (!result) return ids;
    while (result->next()) {
        ids.push_back(result->asUint64(0));
    }
    return ids;
}

std::vector<std::string> PostgreSQLEntityStore::listEntityTypes() {
    std::vector<std::string> types;
    if (!ensureConnected()) return types;

    // information_schema 是 PG 列举表的标准途径；LIKE 中 \_ 转义下划线通配。
    auto result = conn_->query(
        "SELECT table_name FROM information_schema.tables "
        "WHERE table_schema = 'public' AND table_name LIKE 'tbl\\_%'");
    if (!result) return types;
    while (result->next()) {
        std::string name = result->asString(0);
        if (name.starts_with("tbl_")) {
            types.push_back(name.substr(4));
        }
    }
    return types;
}

// ---------------------------------------------------------------------------
// IAccountStore
// ---------------------------------------------------------------------------

bool PostgreSQLEntityStore::queryAccount(const std::string& username,
                                         core::EntityId& outId,
                                         std::string& outPassword) {
    ScopedMsTimer timer([](double ms) {
        pgHistogram("pg_query_account_ms", "PostgreSQL account query latency").observe(ms);
    });
    if (!ensureConnected()) return false;

    auto result = conn_->query(
        "SELECT entity_id, password FROM _account_index WHERE username = $1::varchar",
        {SqlParam::str(username)});
    if (!result || !result->next()) return false;
    outId = result->asUint64(0);
    outPassword = result->asString(1);
    return true;
}

bool PostgreSQLEntityStore::createAccount(const std::string& username,
                                          const std::string& password,
                                          core::EntityId& outId) {
    ScopedMsTimer timer([](double ms) {
        pgHistogram("pg_create_account_ms", "PostgreSQL createAccount latency").observe(ms);
    });
    if (!ensureConnected()) return false;

    // 先查重：username 来自调用方，参数化绑定防 SQL 注入。
    auto dup = conn_->query(
        "SELECT entity_id FROM _account_index WHERE username = $1::varchar",
        {SqlParam::str(username)});
    if (dup && dup->next()) {
        return false;  // 用户名已存在
    }

    outId = allocId();
    if (outId == 0) return false;

    // 构造 Account EntityData 并写入主表
    core::EntityData data;
    data.id = outId;
    data.entityType = "Account";

    core::PropertyData usernameProp;
    usernameProp.id = 0;
    usernameProp.name = "username";
    usernameProp.type = core::DataType::String;
    usernameProp.rawValue.assign(
        reinterpret_cast<const std::byte*>(username.data()),
        reinterpret_cast<const std::byte*>(username.data()) + username.size());
    data.properties.push_back(usernameProp);

    core::PropertyData passwordProp;
    passwordProp.id = 1;
    passwordProp.name = "password";
    passwordProp.type = core::DataType::String;
    passwordProp.rawValue.assign(
        reinterpret_cast<const std::byte*>(password.data()),
        reinterpret_cast<const std::byte*>(password.data()) + password.size());
    data.properties.push_back(passwordProp);

    if (!save(outId, data)) return false;

    // 写索引表
    if (!conn_->execute(
            "INSERT INTO _account_index (username, entity_id, password) "
            "VALUES ($1::varchar, $2::bigint, $3::bytea) "
            "ON CONFLICT (username) DO UPDATE SET entity_id = EXCLUDED.entity_id, "
            "password = EXCLUDED.password",
            {SqlParam::str(username), SqlParam::u64(outId), strToBytes(password)})) {
        lastError_ = "createAccount index insert failed: " + conn_->lastError();
        return false;
    }
    return true;
}

}  // namespace theseed::db
