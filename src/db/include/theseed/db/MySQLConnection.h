#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace theseed::db {

// MySQL C 客户端的连接配置。libmysql 是 theseed 引入的第一个外部依赖，
// 通过 vcpkg manifest 安装（见 vcpkg.json）。本类是对 mysql C API 的薄 RAII 封装，
// 设计目标：
//   1. 用 pimpl 把 <mysql.h> 隔离在 .cpp，避免污染下游头文件
//   2. 提供最小够用的 execute / query / prepared statement 接口
//   3. 支持掉线后自动重连（ping + reconnect）
struct MySQLConnectionConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 3306;
    std::string user = "theseed";
    std::string password;
    std::string database = "theseed";
    std::string charset = "utf8mb4";
    unsigned int connectTimeoutSeconds = 5;
    bool autoReconnect = true;
};

// 一条 SQL 查询结果集的最小封装。仅前向遍历。
// 通过 MySQLConnection::query() 返回，生命周期与 MySQLConnection 绑定。
class MySQLResult {
public:
    MySQLResult();
    ~MySQLResult();

    MySQLResult(const MySQLResult&) = delete;
    MySQLResult& operator=(const MySQLResult&) = delete;
    MySQLResult(MySQLResult&&) noexcept;
    MySQLResult& operator=(MySQLResult&&) noexcept;

    // 前进到下一行。首次调用前游标位于第一行之前。
    // 返回 false 表示已到末尾或结果为空。
    bool next();

    // 当前行指定列（0 基）的字节数据。NULL 列返回空 span。
    // 注意：返回的指针仅在再次调用 next() 或结果集销毁前有效。
    std::span<const std::byte> asBytes(std::size_t col) const;

    // 当前列按字符串读取（按 charset 解码为 UTF-8 字节）。
    std::string asString(std::size_t col) const;

    // 当前列按 uint64 读取。解析失败返回 0。
    std::uint64_t asUint64(std::size_t col) const;

    std::size_t columnCount() const;
    std::size_t rowCount() const;

private:
    friend class MySQLConnection;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit MySQLResult(std::unique_ptr<Impl> impl);
};

// 单连接的 MySQL 句柄。线程不安全——DBApp 是单线程 tick，一个连接足够。
// MVP 不引入连接池；多进程部署时每个 DBApp 各持一个连接。
class MySQLConnection {
public:
    explicit MySQLConnection(MySQLConnectionConfig config);
    ~MySQLConnection();

    MySQLConnection(const MySQLConnection&) = delete;
    MySQLConnection& operator=(const MySQLConnection&) = delete;

    // 建立连接。失败时 lastError() 填充原因。
    bool connect();
    void disconnect();
    bool isConnected() const;

    // 执行无结果集语句（DDL/DML/INSERT/UPDATE/DELETE）。
    bool execute(const std::string& sql);

    // 执行有结果集语句。返回空 optional 表示执行失败（见 lastError）。
    std::optional<MySQLResult> query(const std::string& sql);

    // 预处理语句绑定参数执行（防 SQL 注入）。
    // params 中每个元素对应 sql 中的一个 '?'。二进制安全。
    bool executeParams(std::string_view sql,
                       const std::vector<std::pair<bool, std::vector<std::byte>>>& params);

    // 预处理语句执行带结果集的查询。参数格式同 executeParams。
    // 返回空 optional 表示执行失败（见 lastError）。
    std::optional<MySQLResult> queryParams(
        std::string_view sql,
        const std::vector<std::pair<bool, std::vector<std::byte>>>& params);

    // 查询最后一条 INSERT 的自增 ID。
    std::uint64_t lastInsertId() const;
    // 最近一条语句影响的行数。
    std::uint64_t affectedRows() const;

    // 连接活性探测。失败会按配置尝试自动重连。
    bool ping();
    bool ensureConnected();

    const std::string& lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    MySQLConnectionConfig config_;
};

}  // namespace theseed::db
