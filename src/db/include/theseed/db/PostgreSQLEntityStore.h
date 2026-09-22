#pragma once

#include "theseed/core/IEntityStore.h"
#include "theseed/db/IAccountStore.h"
#include "theseed/db/PostgreSQLConnection.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace theseed::db {

// 基于 PostgreSQL 的实体存储后端，与 MySQLEntityStore 语义一致：
//   - 每个实体类型一张表 tbl_<EntityType>，主键 id，属性序列化为 BYTEA
//   - 账号查询走 _account_index 索引表
//   - 全局 ID 分配走 _entity_ids 表，
//     INSERT ... ON CONFLICT ... DO UPDATE ... RETURNING 单语句原子完成，
//     首个 id 为 1（与 FileEntityStore / MySQLEntityStore 一致）
//
// 与 MySQL 版的差异仅在 SQL 方言（双引号标识符、ON CONFLICT、RETURNING）
// 与 libpq 的参数传递，存储模型与 BLOB 序列化格式完全相同，两个后端
// 读写同一份逻辑数据。
class PostgreSQLEntityStore final : public core::IEntityStore, public IAccountStore {
public:
    struct Config {
        PostgreSQLConnectionConfig pg;
        bool autoCreateSchema = true;
        // 用于测试注入 mock 连接；生产路径留空，内部创建真实 PostgreSQLConnection。
        std::shared_ptr<PostgreSQLConnection> connection;
    };

    explicit PostgreSQLEntityStore(Config config);
    ~PostgreSQLEntityStore() override;

    PostgreSQLEntityStore(const PostgreSQLEntityStore&) = delete;
    PostgreSQLEntityStore& operator=(const PostgreSQLEntityStore&) = delete;

    bool init();

    // --- IEntityStore ---
    bool load(core::EntityId id, const std::string& entityType,
              core::EntityData& out) override;
    bool save(core::EntityId id, const core::EntityData& data) override;
    bool remove(core::EntityId id) override;
    core::EntityId allocId() override;
    std::vector<core::EntityId> listIdsByType(const std::string& entityType) override;
    std::vector<std::string> listEntityTypes() override;

    // --- IAccountStore ---
    bool queryAccount(const std::string& username,
                      core::EntityId& outId,
                      std::string& outPassword) override;
    bool createAccount(const std::string& username,
                       const std::string& password,
                       core::EntityId& outId) override;

    const std::string& lastError() const;

    // 执行原始 SQL（DDL/DML）。仅供运维工具与测试使用。
    bool executeRaw(const std::string& sql);

private:
    bool createSchema();
    bool ensureTable(const std::string& entityType);
    bool ensureConnected();

    // 将 entityType 转为合法的 PostgreSQL 表名：tbl_<sanitized>。
    static std::string tableName(const std::string& entityType);

    Config config_;
    std::shared_ptr<PostgreSQLConnection> conn_;
    std::unordered_set<std::string> knownTables_;
    std::string lastError_;
};

}  // namespace theseed::db
