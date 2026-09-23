#pragma once

#include "theseed/core/IEntityStore.h"
#include "theseed/db/IAccountStore.h"
#include "theseed/db/MySQLConnection.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace theseed::db {

// 基于 MySQL 的实体存储后端。Phase B 目标：把 DBApp 的主持久化从 FileEntityStore
// 升级为 MySQL，对齐 MVP 架构基线（docs/design/0-foundation/01-...md §10）。
//
// MVP 存储模型（简单方案，后续演进）：
//   - 每个实体类型一张表 tbl_<EntityType>，主键 id
//   - 属性整体序列化为二进制 MemoryStream 存 BLOB 列（与 FileEntityStore 同格式）
//   - 账号查询走专用索引表 _account_index（避免线性扫描）
//   - 全局 ID 分配走 _entity_ids 表的原子 UPDATE
//
// 为什么不用 JSON 列 / EXPAND 展开：当前 EntityData 是二进制格式（见
// src/core/EntityData.cpp 的 encodeEntityData），引入 JSON 列需要额外的
// 序列化路径与 schema 迁移，留给 Phase 2 的数据工具链。
class MySQLEntityStore final : public core::IEntityStore, public IAccountStore {
public:
    struct Config {
        MySQLConnectionConfig mysql;
        bool autoCreateSchema = true;
        // 用于测试注入 mock 连接；生产路径留空，内部创建真实 MySQLConnection。
        std::shared_ptr<MySQLConnection> connection;
    };

    explicit MySQLEntityStore(Config config);
    ~MySQLEntityStore() override;

    MySQLEntityStore(const MySQLEntityStore&) = delete;
    MySQLEntityStore& operator=(const MySQLEntityStore&) = delete;

    // 建立连接并按需建表。任何 store 操作前必须调用。
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

    // 执行原始 SQL（DDL/DML），无结果集返回。仅供运维工具与测试使用，
    // 不进入运行时 CRUD 主路径。调用方需保证 sql 已做转义。
    bool executeRaw(const std::string& sql);

private:
    bool createSchema();
    bool ensureTable(const std::string& entityType);
    bool ensureConnected();

    // 将 entityType 转为合法的 MySQL 表名：tbl_<sanitized>。
    // 仅保留字母数字下划线，其余替换为下划线，避免 SQL 注入。
    static std::string tableName(const std::string& entityType);

    Config config_;
    std::shared_ptr<MySQLConnection> conn_;
    std::unordered_set<std::string> knownTables_;
    std::string lastError_;
};

}  // namespace theseed::db
