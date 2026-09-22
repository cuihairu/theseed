#pragma once

#include "theseed/db/SqlParam.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace theseed::db {

// libpq 连接配置。libpq 通过 vcpkg manifest 安装（见 vcpkg.json）。
// 与 MySQLConnection 同为对 C API 的薄 RAII 封装，用 pimpl 把 libpq 头
// 隔离在 .cpp。
struct PostgreSQLConnectionConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 5432;
    std::string user = "theseed";
    std::string password;
    std::string database = "theseed";
    unsigned int connectTimeoutSeconds = 5;
};

// 一条 SQL 查询结果集的最小封装。仅前向遍历，语义与 MySQLResult 一致。
// bytea 列在 PQgetvalue 返回的是十六进制文本，此处已解码为原始字节，
// asBytes 拿到的就是存入时的二进制。
class PostgreSQLResult {
public:
    PostgreSQLResult();
    ~PostgreSQLResult();

    PostgreSQLResult(const PostgreSQLResult&) = delete;
    PostgreSQLResult& operator=(const PostgreSQLResult&) = delete;
    PostgreSQLResult(PostgreSQLResult&&) noexcept;
    PostgreSQLResult& operator=(PostgreSQLResult&&) noexcept;

    bool next();

    // 当前行指定列（0 基）的字节数据。NULL 列返回空 span。
    // 返回的指针仅在再次调用 next() 或结果集销毁前有效。
    std::span<const std::byte> asBytes(std::size_t col) const;

    std::string asString(std::size_t col) const;

    // 当前列按 uint64 读取。解析失败返回 0。
    std::uint64_t asUint64(std::size_t col) const;

    std::size_t columnCount() const;
    std::size_t rowCount() const;

private:
    friend class PostgreSQLConnection;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit PostgreSQLResult(std::unique_ptr<Impl> impl);
};

// 单连接的 libpq 句柄。线程不安全——DBApp 是单线程 tick，一个连接足够。
// libpq 的 PQexecParams 一步完成"解析 + 绑定 + 执行"，无需显式 prepare。
// 参数统一走 text 格式：整数转十进制字符串，字节串转 \x 十六进制
// （配合 SQL 里的 ::bytea / ::bigint 显式类型标注），规避 NUL 截断。
class PostgreSQLConnection {
public:
    explicit PostgreSQLConnection(PostgreSQLConnectionConfig config);
    ~PostgreSQLConnection();

    PostgreSQLConnection(const PostgreSQLConnection&) = delete;
    PostgreSQLConnection& operator=(const PostgreSQLConnection&) = delete;

    // 建立连接。失败时 lastError() 填充原因。
    bool connect();
    void disconnect();
    bool isConnected() const;

    // 执行无结果集语句（DDL/DML）。params 为空时走无参路径。
    bool execute(std::string_view sql, const std::vector<SqlParam>& params = {});

    // 执行带结果集的查询。返回空 optional 表示执行失败（见 lastError）。
    std::optional<PostgreSQLResult> query(std::string_view sql,
                                          const std::vector<SqlParam>& params = {});

    // 最近一条语句影响的行数。
    std::uint64_t affectedRows() const;

    // 连接活性探测。掉线时按 PQreset 重连一次。
    bool ping();
    bool ensureConnected();

    const std::string& lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    PostgreSQLConnectionConfig config_;
};

}  // namespace theseed::db
