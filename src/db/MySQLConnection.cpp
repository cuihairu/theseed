#include "theseed/db/MySQLConnection.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mysql/mysql.h>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
// vcpkg libmysql port 全平台统一安装到 include/mysql/（INSTALL_INCLUDEDIR），
// 因此使用 mysql/mysql.h 而非裸 mysql.h。

namespace theseed::db {

namespace {

// 把一个字节数据库列值转成 uint64（十进制字符串解析）。
std::uint64_t bytesToUint64(std::span<const std::byte> data) {
    if (data.empty()) return 0;
    std::uint64_t value = 0;
    for (auto b : data) {
        char c = static_cast<char>(b);
        if (c < '0' || c > '9') break;
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return value;
}

}  // namespace

// ---------------------------------------------------------------------------
// MySQLResult::Impl
// 支持两种结果来源：
//   1. 普通查询：持有 MYSQL_RES*，逐行 mysql_fetch_row
//   2. prepared 查询：把 stmt 的结果预先 fetch 成内存中的二维字节表
// ---------------------------------------------------------------------------

struct MySQLResult::Impl {
    // 来源 1：普通查询
    MYSQL_RES* res = nullptr;
    MYSQL_ROW currentRow = nullptr;
    unsigned long* lengths = nullptr;

    // 来源 2：materialized 行（prepared 查询）
    std::vector<std::vector<std::vector<std::byte>>> rows;
    std::size_t cursor = 0;  // 当前已消费到的行索引（next 后指向的行）

    std::size_t columnCountVal = 0;
    std::size_t rowCountVal = 0;
    bool materialized = false;  // true 表示走 rows 路径

    explicit Impl(MYSQL_RES* r) : res(r) {
        if (res != nullptr) {
            columnCountVal = static_cast<std::size_t>(mysql_num_fields(res));
            rowCountVal = static_cast<std::size_t>(mysql_num_rows(res));
        }
    }

    // prepared 查询的 materialized 结果构造
    Impl(std::vector<std::vector<std::vector<std::byte>>> r,
         std::size_t colCount)
        : rows(std::move(r)),
          columnCountVal(colCount),
          rowCountVal(rows.size()),
          materialized(true) {}

    ~Impl() {
        if (res != nullptr) mysql_free_result(res);
    }

    bool advance() {
        if (res == nullptr) return false;
        currentRow = mysql_fetch_row(res);
        lengths = mysql_fetch_lengths(res);
        return currentRow != nullptr;
    }
};

MySQLResult::MySQLResult() = default;

MySQLResult::MySQLResult(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

MySQLResult::~MySQLResult() = default;

MySQLResult::MySQLResult(MySQLResult&&) noexcept = default;
MySQLResult& MySQLResult::operator=(MySQLResult&&) noexcept = default;

bool MySQLResult::next() {
    if (!impl_) return false;
    if (impl_->materialized) {
        // cursor 表示"下一次 next 要取的行"（0-based）
        if (impl_->cursor >= impl_->rows.size()) return false;
        ++impl_->cursor;
        return true;
    }
    return impl_->advance();
}

std::span<const std::byte> MySQLResult::asBytes(std::size_t col) const {
    if (!impl_) return {};
    if (impl_->materialized) {
        // cursor-1 是当前已消费行（next 把它 +1）
        if (impl_->cursor == 0 || impl_->cursor > impl_->rows.size()) return {};
        const auto& row = impl_->rows[impl_->cursor - 1];
        if (col >= row.size()) return {};
        return {row[col].data(), row[col].size()};
    }
    if (impl_->currentRow == nullptr) return {};
    if (col >= impl_->columnCountVal) return {};
    const char* cell = impl_->currentRow[col];
    if (cell == nullptr) return {};  // SQL NULL
    unsigned long len = impl_->lengths ? impl_->lengths[col] : 0;
    return {reinterpret_cast<const std::byte*>(cell), static_cast<std::size_t>(len)};
}

std::string MySQLResult::asString(std::size_t col) const {
    auto bytes = asBytes(col);
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::uint64_t MySQLResult::asUint64(std::size_t col) const {
    return bytesToUint64(asBytes(col));
}

std::size_t MySQLResult::columnCount() const {
    return impl_ ? impl_->columnCountVal : 0;
}

std::size_t MySQLResult::rowCount() const {
    return impl_ ? impl_->rowCountVal : 0;
}

// ---------------------------------------------------------------------------
// MySQLConnection::Impl
// ---------------------------------------------------------------------------

struct MySQLConnection::Impl {
    MYSQL mysql;
    bool connected = false;
    mutable std::string lastError;
    std::uint64_t lastInsertIdVal = 0;
    std::uint64_t affectedRowsVal = 0;

    Impl() {
        mysql_init(&mysql);
    }
    ~Impl() {
        if (connected) mysql_close(&mysql);
    }

    void captureError() {
        lastError = mysql_error(&mysql);
    }
};

MySQLConnection::MySQLConnection(MySQLConnectionConfig config)
    : impl_(std::make_unique<Impl>()), config_(std::move(config)) {}

MySQLConnection::~MySQLConnection() = default;

bool MySQLConnection::connect() {
    if (impl_->connected) return true;

    // 重新初始化以防之前 close 过
    if (!mysql_init(&impl_->mysql)) {
        impl_->lastError = "mysql_init failed";
        return false;
    }

    mysql_options(&impl_->mysql, MYSQL_OPT_CONNECT_TIMEOUT,
                  &config_.connectTimeoutSeconds);
    if (!config_.charset.empty()) {
        mysql_options(&impl_->mysql, MYSQL_SET_CHARSET_NAME, config_.charset.c_str());
    }
    if (config_.autoReconnect) {
        // MySQL 8.0 客户端头中该选项为 bool*（旧 my_bool 已移除）
        bool reconnect = true;
        mysql_options(&impl_->mysql, MYSQL_OPT_RECONNECT, &reconnect);
    }

    if (mysql_real_connect(&impl_->mysql,
                           config_.host.c_str(),
                           config_.user.c_str(),
                           config_.password.c_str(),
                           config_.database.c_str(),
                           config_.port,
                           nullptr,                   // unix socket
                           CLIENT_MULTI_STATEMENTS) == nullptr) {
        impl_->captureError();
        impl_->connected = false;
        return false;
    }

    impl_->connected = true;
    return true;
}

void MySQLConnection::disconnect() {
    if (impl_->connected) {
        mysql_close(&impl_->mysql);
        // mysql_close 后 MYSQL 结构失效，需重新 init 以便再次 connect
        mysql_init(&impl_->mysql);
        impl_->connected = false;
    }
}

bool MySQLConnection::isConnected() const {
    return impl_->connected;
}

bool MySQLConnection::execute(const std::string& sql) {
    if (!ensureConnected()) return false;
    if (mysql_real_query(&impl_->mysql, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
        impl_->captureError();
        return false;
    }
    impl_->affectedRowsVal = static_cast<std::uint64_t>(mysql_affected_rows(&impl_->mysql));
    impl_->lastInsertIdVal = static_cast<std::uint64_t>(mysql_insert_id(&impl_->mysql));
    // 对于多语句查询，排空可能存在的多余结果集，避免污染下一次 query
    MYSQL_RES* extra = mysql_store_result(&impl_->mysql);
    if (extra != nullptr) mysql_free_result(extra);
    while (mysql_next_result(&impl_->mysql) == 0) {
        MYSQL_RES* more = mysql_store_result(&impl_->mysql);
        if (more != nullptr) mysql_free_result(more);
    }
    return true;
}

std::optional<MySQLResult> MySQLConnection::query(const std::string& sql) {
    if (!ensureConnected()) return std::nullopt;
    if (mysql_real_query(&impl_->mysql, sql.c_str(), static_cast<unsigned long>(sql.size())) != 0) {
        impl_->captureError();
        return std::nullopt;
    }
    MYSQL_RES* res = mysql_store_result(&impl_->mysql);
    if (res == nullptr) {
        // 非查询语句或出错
        impl_->captureError();
        return std::nullopt;
    }
    impl_->affectedRowsVal = static_cast<std::uint64_t>(mysql_affected_rows(&impl_->mysql));
    return MySQLResult(std::make_unique<MySQLResult::Impl>(res));
}

bool MySQLConnection::executeParams(std::string_view sql,
                                    const std::vector<MySqlParam>& params) {
    if (!ensureConnected()) return false;

    MYSQL_STMT* stmt = mysql_stmt_init(&impl_->mysql);
    if (stmt == nullptr) {
        impl_->lastError = "mysql_stmt_init failed";
        return false;
    }

    auto cleanup = [this, stmt]() {
        mysql_stmt_close(stmt);
    };

    if (mysql_stmt_prepare(stmt, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
        impl_->lastError = mysql_stmt_error(stmt);
        cleanup();
        return false;
    }

    unsigned long paramCount = mysql_stmt_param_count(stmt);
    if (paramCount != params.size()) {
        std::ostringstream oss;
        oss << "param count mismatch: sql expects " << paramCount
            << ", got " << params.size();
        impl_->lastError = oss.str();
        cleanup();
        return false;
    }

    std::vector<MYSQL_BIND> binds(params.size());
    std::vector<unsigned long> lengths(params.size());
    // 每个 is_null 标志必须存活到 execute 之后。MySQL 8.0 头中 is_null 为
    // bool*，且 std::vector<bool> 元素取不出真实地址，故用动态 bool 数组。
    std::unique_ptr<bool[]> nullFlags(new bool[params.size()]());
    std::memset(binds.data(), 0, sizeof(MYSQL_BIND) * binds.size());

    for (std::size_t i = 0; i < params.size(); ++i) {
        const auto& p = params[i];
        nullFlags[i] = p.isNull;
        lengths[i] = static_cast<unsigned long>(p.bytes.size());
        binds[i].is_null = &nullFlags[i];
        if (p.isUint64) {
            // 整数按真实类型绑定，避免二进制串进 BIGINT 被严格模式拒绝。
            // buffer 指向 params 内的存储，存活至 execute 之后。
            binds[i].buffer_type = MYSQL_TYPE_LONGLONG;
            binds[i].buffer = const_cast<unsigned char*>(
                reinterpret_cast<const unsigned char*>(&p.uint64Value));
            binds[i].buffer_length = sizeof(p.uint64Value);
            binds[i].is_unsigned = true;
        } else {
            binds[i].buffer_type = MYSQL_TYPE_BLOB;  // 统一按二进制串绑定，避免类型转换
            binds[i].buffer =
                const_cast<char*>(reinterpret_cast<const char*>(p.bytes.data()));
            binds[i].buffer_length = lengths[i];
            binds[i].length = &lengths[i];
        }
    }

    if (mysql_stmt_bind_param(stmt, binds.data()) != 0) {
        impl_->lastError = mysql_stmt_error(stmt);
        cleanup();
        return false;
    }

    if (mysql_stmt_execute(stmt) != 0) {
        impl_->lastError = mysql_stmt_error(stmt);
        cleanup();
        return false;
    }

    impl_->affectedRowsVal = static_cast<std::uint64_t>(mysql_stmt_affected_rows(stmt));
    impl_->lastInsertIdVal = static_cast<std::uint64_t>(mysql_stmt_insert_id(stmt));

    // 排空结果集（INSERT/UPDATE 通常无结果集，但 SELECT ... 会有）
    mysql_stmt_free_result(stmt);
    cleanup();
    return true;
}

std::optional<MySQLResult> MySQLConnection::queryParams(std::string_view sql,
                                                        const std::vector<MySqlParam>& params) {
    if (!ensureConnected()) return std::nullopt;

    MYSQL_STMT* stmt = mysql_stmt_init(&impl_->mysql);
    if (stmt == nullptr) {
        impl_->lastError = "mysql_stmt_init failed";
        return std::nullopt;
    }

    if (mysql_stmt_prepare(stmt, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
        impl_->lastError = mysql_stmt_error(stmt);
        mysql_stmt_close(stmt);
        return std::nullopt;
    }

    // 注意：这三个容器必须存活到 mysql_stmt_execute 之后——bind_param 只保存
    // 指向它们的指针而不拷贝内容，若声明在下方 if 块内，块结束即析构，
    // execute 会读到悬垂栈内存（曾导致参数被当作 NULL 发送，静默匹配 0 行）。
    std::vector<MYSQL_BIND> binds(params.size());
    std::vector<unsigned long> lengths(params.size());
    // MySQL 8.0 头中 is_null 为 bool*，且 std::vector<bool> 元素取不出真实地址。
    std::unique_ptr<bool[]> nullFlags(new bool[params.size()]());
    std::memset(binds.data(), 0, sizeof(MYSQL_BIND) * binds.size());

    if (!params.empty()) {
        unsigned long paramCount = mysql_stmt_param_count(stmt);
        if (paramCount != params.size()) {
            std::ostringstream oss;
            oss << "param count mismatch: sql expects " << paramCount
                << ", got " << params.size();
            impl_->lastError = oss.str();
            mysql_stmt_close(stmt);
            return std::nullopt;
        }

        for (std::size_t i = 0; i < params.size(); ++i) {
            const auto& p = params[i];
            nullFlags[i] = p.isNull;
            lengths[i] = static_cast<unsigned long>(p.bytes.size());
            binds[i].is_null = &nullFlags[i];
            if (p.isUint64) {
                binds[i].buffer_type = MYSQL_TYPE_LONGLONG;
                binds[i].buffer = const_cast<unsigned char*>(
                    reinterpret_cast<const unsigned char*>(&p.uint64Value));
                binds[i].buffer_length = sizeof(p.uint64Value);
                binds[i].is_unsigned = true;
            } else {
                binds[i].buffer_type = MYSQL_TYPE_BLOB;
                binds[i].buffer =
                    const_cast<char*>(reinterpret_cast<const char*>(p.bytes.data()));
                binds[i].buffer_length = lengths[i];
                binds[i].length = &lengths[i];
            }
        }

        if (mysql_stmt_bind_param(stmt, binds.data()) != 0) {
            impl_->lastError = mysql_stmt_error(stmt);
            mysql_stmt_close(stmt);
            return std::nullopt;
        }
    }

    if (mysql_stmt_execute(stmt) != 0) {
        impl_->lastError = mysql_stmt_error(stmt);
        mysql_stmt_close(stmt);
        return std::nullopt;
    }

    // 把结果集拉到客户端，以便逐行 fetch
    if (mysql_stmt_store_result(stmt) != 0) {
        impl_->lastError = mysql_stmt_error(stmt);
        mysql_stmt_close(stmt);
        return std::nullopt;
    }

    MYSQL_RES* meta = mysql_stmt_result_metadata(stmt);
    if (meta == nullptr) {
        // 非查询语句
        impl_->affectedRowsVal = static_cast<std::uint64_t>(mysql_stmt_affected_rows(stmt));
        mysql_stmt_free_result(stmt);
        mysql_stmt_close(stmt);
        return std::nullopt;
    }

    std::size_t colCount = static_cast<std::size_t>(mysql_num_fields(meta));

    // 绑定输出：每列用一个动态缓冲，处理 truncation。
    constexpr std::size_t kFetchBuf = 65536;  // 64KB per column per fetch
    std::vector<std::vector<std::byte>> colBufs(colCount);
    std::vector<unsigned long> colLens(colCount, 0);
    std::unique_ptr<bool[]> colNulls(new bool[colCount]());
    std::vector<MYSQL_BIND> outBinds(colCount);
    std::memset(outBinds.data(), 0, sizeof(MYSQL_BIND) * colCount);
    for (std::size_t i = 0; i < colCount; ++i) {
        colBufs[i].resize(kFetchBuf);
        outBinds[i].buffer_type = MYSQL_TYPE_BLOB;
        outBinds[i].buffer = reinterpret_cast<char*>(colBufs[i].data());
        outBinds[i].buffer_length = kFetchBuf;
        outBinds[i].length = &colLens[i];
        outBinds[i].is_null = &colNulls[i];
    }

    if (mysql_stmt_bind_result(stmt, outBinds.data()) != 0) {
        impl_->lastError = mysql_stmt_error(stmt);
        mysql_free_result(meta);
        mysql_stmt_close(stmt);
        return std::nullopt;
    }

    // 逐行 fetch 到内存
    std::vector<std::vector<std::vector<std::byte>>> rows;
    while (true) {
        int rc = mysql_stmt_fetch(stmt);
        if (rc == 1) {
            impl_->lastError = mysql_stmt_error(stmt);
            break;
        }
        if (rc == MYSQL_NO_DATA) break;

        std::vector<std::vector<std::byte>> row(colCount);
        for (std::size_t i = 0; i < colCount; ++i) {
            if (colNulls[i]) continue;  // NULL
            std::size_t actualLen = static_cast<std::size_t>(colLens[i]);
            if (actualLen > kFetchBuf) {
                // 截断：实际值超过单列缓冲。MVP 实体 blob 通常远小于 64KB，
                // 走到这里说明配置不当（应调大 max_allowed_packet 或拆属性）。
                // 用 mysql_stmt_fetch_column 把剩余部分补齐。
                row[i].assign(colBufs[i].begin(), colBufs[i].begin() + kFetchBuf);
                std::size_t remaining = actualLen - kFetchBuf;
                unsigned long offset = static_cast<unsigned long>(kFetchBuf);
                std::vector<char> tail(remaining);
                unsigned long got = 0;
                while (got < remaining) {
                    MYSQL_BIND part{};
                    part.buffer_type = MYSQL_TYPE_BLOB;
                    part.buffer = tail.data() + got;
                    part.buffer_length = static_cast<unsigned long>(remaining - got);
                    unsigned long partLen = 0;
                    part.length = &partLen;
                    if (mysql_stmt_fetch_column(stmt, &part,
                                                 static_cast<unsigned int>(i),
                                                 offset + got) != 0) {
                        break;
                    }
                    // libmysql 语义：*length 回填的是列值全长而非本次拷贝字节数，
                    // 实际拷贝 min(全长 - offset, 缓冲容量) 字节。按全长累加会
                    // 越过 tail 缓冲导致 insert 越界读堆内存。
                    if (partLen <= offset + got) break;
                    const unsigned long copied =
                        std::min(partLen - offset - got,
                                 static_cast<unsigned long>(remaining - got));
                    if (copied == 0) break;
                    got += copied;
                }
                row[i].insert(row[i].end(),
                              reinterpret_cast<const std::byte*>(tail.data()),
                              reinterpret_cast<const std::byte*>(tail.data()) + got);
            } else {
                row[i].assign(
                    reinterpret_cast<const std::byte*>(colBufs[i].data()),
                    reinterpret_cast<const std::byte*>(colBufs[i].data()) + actualLen);
            }
        }
        rows.push_back(std::move(row));
    }

    impl_->affectedRowsVal = static_cast<std::uint64_t>(mysql_stmt_affected_rows(stmt));
    mysql_free_result(meta);
    mysql_stmt_free_result(stmt);
    mysql_stmt_close(stmt);

    return MySQLResult(std::make_unique<MySQLResult::Impl>(std::move(rows), colCount));
}

std::uint64_t MySQLConnection::lastInsertId() const {
    return impl_->lastInsertIdVal;
}

std::uint64_t MySQLConnection::affectedRows() const {
    return impl_->affectedRowsVal;
}

bool MySQLConnection::ping() {
    if (!impl_->connected) return false;
    return mysql_ping(&impl_->mysql) == 0;
}

bool MySQLConnection::ensureConnected() {
    if (impl_->connected && mysql_ping(&impl_->mysql) == 0) {
        return true;
    }
    impl_->connected = false;
    if (!config_.autoReconnect) return false;
    // 重新 init 后重连
    mysql_init(&impl_->mysql);
    return connect();
}

const std::string& MySQLConnection::lastError() const {
    return impl_->lastError;
}

}  // namespace theseed::db
