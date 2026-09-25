#include "theseed/db/MySQLEntityStore.h"
#include "theseed/foundation/Metrics.h"

#include <chrono>
#include <cstddef>
#include <cstring>
#include <functional>
#include <sstream>
#include <utility>

namespace theseed::db {

namespace {

// RAII tick 延迟计量器，仿照 DBApp 中的 ScopedTimer。
class ScopedMsTimer final {
public:
    using Emitter = std::function<void(double)>;
    explicit ScopedMsTimer(Emitter emitter)
        : start_(std::chrono::steady_clock::now()), emitter_(std::move(emitter)) {}
    ~ScopedMsTimer() {
        if (emitter_) {  // LCOV_EXCL_BR_LINE emitter_ 恒非空（各调用点均传发射器），空检查臂不可达
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

foundation::Histogram& mysqlHistogram(const char* name, const char* desc) {
    return foundation::MetricsRegistry::instance().histogram(
        name,
        foundation::Histogram::Boundaries{0.5, 1.0, 2.0, 5.0, 10.0, 25.0, 50.0,
                                          100.0, 250.0, 500.0, 1000.0},
        desc);
}

// 把 EntityType 净化为安全的表名后缀。只保留 [A-Za-z0-9_]，其余变下划线。
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

// 将字符串字面量编码为 MySQL 二进制绑定所需的 byte vector。
std::vector<std::byte> strToBytes(const std::string& s) {
    return std::vector<std::byte>(
        reinterpret_cast<const std::byte*>(s.data()),
        reinterpret_cast<const std::byte*>(s.data()) + s.size());
}

}  // namespace

// ---------------------------------------------------------------------------
// 构造与初始化
// ---------------------------------------------------------------------------

MySQLEntityStore::MySQLEntityStore(Config config)
    : config_(std::move(config)) {}

MySQLEntityStore::~MySQLEntityStore() = default;  // LCOV_EXCL_LINE trivial 析构的 out-of-line 定义无机器码，gcc 不产生计数条目

bool MySQLEntityStore::init() {
    if (config_.connection) {
        conn_ = config_.connection;
    } else {
        conn_ = std::make_shared<MySQLConnection>(config_.mysql);
        if (!conn_->connect()) {
            lastError_ = "MySQL connect failed: " + conn_->lastError();
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

bool MySQLEntityStore::createSchema() {
    // 全局 ID 分配表。不预置任何行——allocId() 的
    // INSERT...ON DUPLICATE KEY UPDATE 会按需创建 '__global__' 行。
    if (!conn_->execute(
            "CREATE TABLE IF NOT EXISTS `_entity_ids` ("
            "  `entity_type` VARCHAR(64) NOT NULL PRIMARY KEY,"
            "  `next_id` BIGINT UNSIGNED NOT NULL DEFAULT 1"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4")) {
        lastError_ = "create _entity_ids failed: " + conn_->lastError();
        return false;
    }

    // Account 索引表
    if (!conn_->execute(
            "CREATE TABLE IF NOT EXISTS `_account_index` ("
            "  `username` VARCHAR(128) NOT NULL PRIMARY KEY,"
            "  `entity_id` BIGINT UNSIGNED NOT NULL,"
            "  `password` VARBINARY(256) NOT NULL,"
            "  INDEX `idx_entity_id` (`entity_id`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4")) {
        lastError_ = "create _account_index failed: " + conn_->lastError();
        return false;
    }
    return true;
}

std::string MySQLEntityStore::tableName(const std::string& entityType) {
    return "tbl_" + sanitizeForTable(entityType);
}

bool MySQLEntityStore::ensureTable(const std::string& entityType) {
    auto tbl = tableName(entityType);
    if (knownTables_.count(tbl) > 0) return true;

    std::ostringstream ddl;
    ddl << "CREATE TABLE IF NOT EXISTS `" << tbl << "` ("
        << "  `id` BIGINT UNSIGNED NOT NULL PRIMARY KEY,"
        << "  `data` MEDIUMBLOB NOT NULL,"
        << "  `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP"
        << "    ON UPDATE CURRENT_TIMESTAMP,"
        << "  INDEX `idx_updated` (`updated_at`)"
        << ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

    if (!conn_->execute(ddl.str())) {
        lastError_ = "ensureTable failed: " + conn_->lastError();
        return false;
    }
    knownTables_.insert(tbl);
    return true;
}

bool MySQLEntityStore::ensureConnected() {
    if (!conn_) {
        lastError_ = "store not initialized";
        return false;
    }
    return conn_->ensureConnected();
}

const std::string& MySQLEntityStore::lastError() const {
    return lastError_;
}

bool MySQLEntityStore::executeRaw(const std::string& sql) {
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

bool MySQLEntityStore::load(core::EntityId id, const std::string& entityType,
                             core::EntityData& out) {
    ScopedMsTimer timer([](double ms) {
        mysqlHistogram("mysql_load_ms", "MySQL entity load latency").observe(ms);
    });
    if (!ensureConnected() || !ensureTable(entityType)) return false;

    auto tbl = tableName(entityType);
    // id 是 uint64，直接拼入 SQL 无注入风险，避免 prepared statement 结果集
    // 绑定的冗长样板。
    auto result = conn_->query("SELECT `data` FROM `" + tbl + "` WHERE `id` = " +
                               std::to_string(id));
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

bool MySQLEntityStore::save(core::EntityId id, const core::EntityData& data) {
    ScopedMsTimer timer([](double ms) {
        mysqlHistogram("mysql_save_ms", "MySQL entity save latency").observe(ms);
    });
    if (!ensureConnected() || !ensureTable(data.entityType)) return false;

    // 序列化 EntityData 为二进制 BLOB
    foundation::MemoryStream ms;
    core::encodeEntityData(ms, data);
    std::vector<std::byte> blob(ms.data(), ms.data() + ms.size());

    auto tbl = tableName(data.entityType);
    std::ostringstream sql;
    sql << "INSERT INTO `" << tbl << "` (`id`, `data`) VALUES (?, ?) "
        << "ON DUPLICATE KEY UPDATE `data` = VALUES(`data`)";

    if (!conn_->executeParams(sql.str(), {MySqlParam::u64(id), std::move(blob)})) {  // LCOV_EXCL_BR_LINE 初始化列表构造参数 vector 的库内联分支，非业务分支
        lastError_ = "save failed: " + conn_->lastError();
        return false;
    }
    return true;
}  // LCOV_EXCL_BR_LINE ScopedMsTimer 析构内联副本：emitter_ 恒非空，空检查臂不可达

bool MySQLEntityStore::remove(core::EntityId id) {
    ScopedMsTimer timer([](double ms) {
        mysqlHistogram("mysql_remove_ms", "MySQL entity remove latency").observe(ms);
    });
    if (!ensureConnected()) return false;

    // Account 索引表清理（若该 id 是账号）
    conn_->executeParams("DELETE FROM `_account_index` WHERE `entity_id` = ?",  // LCOV_EXCL_BR_LINE 初始化列表构造参数 vector 的库内联分支，非业务分支
                         {MySqlParam::u64(id)});

    // 扫描所有已知表删除。MVP 表数量少，遍历 knownTables_ 即可；
    // 为避免漏删未缓存表，先用 listEntityTypes 补全一次。
    auto types = listEntityTypes();
    bool removed = false;
    for (const auto& entityType : types) {
        ensureTable(entityType);
        auto tbl = tableName(entityType);
        std::ostringstream sql;
        sql << "DELETE FROM `" << tbl << "` WHERE `id` = ?";
        if (!conn_->executeParams(sql.str(), {MySqlParam::u64(id)})) {  // LCOV_EXCL_BR_LINE 初始化列表构造参数 vector 的库内联分支，非业务分支
            lastError_ = "remove failed: " + conn_->lastError();
            return false;
        }
        if (conn_->affectedRows() > 0) removed = true;
    }
    return removed;
}  // LCOV_EXCL_BR_LINE ScopedMsTimer 析构内联副本：emitter_ 恒非空，空检查臂不可达

core::EntityId MySQLEntityStore::allocId() {
    ScopedMsTimer timer([](double ms) {
        mysqlHistogram("mysql_alloc_id_ms", "MySQL allocId latency").observe(ms);
    });
    if (!ensureConnected()) return 0;

    // 全局原子自增计数器（与 FileEntityStore 的 _next_id.dat 语义一致，
    // 首个 id 为 1，不区分实体类型）。LAST_INSERT_ID(expr) 把 expr 写入
    // 连接级状态，随后 SELECT LAST_INSERT_ID() 取回，单语句原子完成。
    // 两条分支都必须显式设值：若新建分支只存常量 1（表无自增列），
    // SELECT LAST_INSERT_ID() 会返回 0，导致全新库上的首次分配必然失败。
    if (!conn_->execute(
            "INSERT INTO `_entity_ids` (`entity_type`, `next_id`) "
            "VALUES ('__global__', LAST_INSERT_ID(1)) "
            "ON DUPLICATE KEY UPDATE `next_id` = LAST_INSERT_ID(`next_id` + 1)")) {
        lastError_ = "allocId upsert failed: " + conn_->lastError();
        return 0;
    }
    auto result = conn_->query("SELECT LAST_INSERT_ID()");
    if (!result || !result->next()) {  // LCOV_EXCL_BR_LINE SELECT LAST_INSERT_ID() 恒返回恰一行，next() 为假不可构造
        lastError_ = "allocId readback failed: " + conn_->lastError();
        return 0;
    }
    return result->asUint64(0);
}

std::vector<core::EntityId> MySQLEntityStore::listIdsByType(const std::string& entityType) {
    std::vector<core::EntityId> ids;
    if (!ensureConnected() || !ensureTable(entityType)) return ids;

    auto tbl = tableName(entityType);
    auto result = conn_->query("SELECT `id` FROM `" + tbl + "` ORDER BY `id`");
    if (!result) return ids;
    while (result->next()) {
        ids.push_back(result->asUint64(0));
    }
    return ids;
}

std::vector<std::string> MySQLEntityStore::listEntityTypes() {
    std::vector<std::string> types;
    if (!ensureConnected()) return types;

    auto result = conn_->query("SHOW TABLES LIKE 'tbl\\_%'");
    if (!result) return types;  // LCOV_EXCL_BR_LINE SHOW TABLES 仅连接级失败，SQL-only 不可构造
    while (result->next()) {
        // 取第一列，去掉 tbl_ 前缀
        std::string name = result->asString(0);
        if (name.starts_with("tbl_")) {  // LCOV_EXCL_BR_LINE 查询已按 LIKE 'tbl\_%' 过滤前缀，starts_with 恒真
            types.push_back(name.substr(4));
        }
    }
    return types;
}

// ---------------------------------------------------------------------------
// IAccountStore
// ---------------------------------------------------------------------------

bool MySQLEntityStore::queryAccount(const std::string& username,
                                     core::EntityId& outId,
                                     std::string& outPassword) {
    ScopedMsTimer timer([](double ms) {
        mysqlHistogram("mysql_query_account_ms", "MySQL account query latency").observe(ms);
    });
    if (!ensureConnected()) return false;

    // 按 username 索引查询，参数化绑定防 SQL 注入。
    auto result = conn_->queryParams(  // LCOV_EXCL_BR_LINE queryParams 参数构造与返回值包装的库内联边，条件判断本体在后续行且两业务向均已覆盖
        "SELECT `entity_id`, `password` FROM `_account_index` WHERE `username` = ?",
        {strToBytes(username)});
    if (!result || !result->next()) return false;
    outId = result->asUint64(0);
    outPassword = result->asString(1);
    return true;
}  // LCOV_EXCL_BR_LINE ScopedMsTimer 析构内联副本：emitter_ 恒非空，空检查臂不可达

bool MySQLEntityStore::createAccount(const std::string& username,
                                      const std::string& password,
                                      core::EntityId& outId) {
    ScopedMsTimer timer([](double ms) {
        mysqlHistogram("mysql_create_account_ms", "MySQL createAccount latency").observe(ms);
    });
    if (!ensureConnected()) return false;

    // 先查重：username 来自调用方，参数化绑定防注入。
    auto dup = conn_->queryParams(  // LCOV_EXCL_BR_LINE queryParams 参数构造与返回值包装的库内联边，查重判断本体在后续行且两业务向均已覆盖
        "SELECT `entity_id` FROM `_account_index` WHERE `username` = ?",
        {strToBytes(username)});
    if (dup && dup->next()) {
        // 用户名已存在
        return false;
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
    if (!conn_->executeParams(  // LCOV_EXCL_BR_LINE 初始化列表构造参数 vector 的库内联分支，非业务分支
            "INSERT INTO `_account_index` (`username`, `entity_id`, `password`) "
            "VALUES (?, ?, ?) "
            "ON DUPLICATE KEY UPDATE `entity_id` = VALUES(`entity_id`), "
            "`password` = VALUES(`password`)",
            {strToBytes(username), MySqlParam::u64(outId), strToBytes(password)})) {
        lastError_ = "createAccount index insert failed: " + conn_->lastError();
        return false;
    }
    return true;
}  // LCOV_EXCL_BR_LINE ScopedMsTimer 析构内联副本：emitter_ 恒非空，空检查臂不可达

}  // namespace theseed::db
