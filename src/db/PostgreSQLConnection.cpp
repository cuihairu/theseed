#include "theseed/db/PostgreSQLConnection.h"

#include <libpq-fe.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace theseed::db {

namespace {

// 十进制字符串解析为 uint64（text 协议下整数列以字符串形式返回）。
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

// 字节串编码为 PG bytea 的十六进制文本（text 格式参数不能内含 NUL）。
std::string bytesToHex(std::span<const std::byte> data) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(2 + data.size() * 2);
    out += "\\x";
    for (auto b : data) {
        unsigned char v = static_cast<unsigned char>(b);
        out.push_back(kHex[v >> 4]);
        out.push_back(kHex[v & 0x0F]);
    }
    return out;
}

std::string u64ToDec(std::uint64_t v) { return std::to_string(v); }

// bytea 类型的 OID。PG 18 起客户端头不再导出 BYTEAOID（移入了 server 头
// catalog/pg_type_d.h），而 OID 是目录表冻结常量，跨版本恒为 17，
// 故此处本地定义，避免依赖 server 头安装。
constexpr Oid kByteaOid = 17;

}  // namespace

// ---------------------------------------------------------------------------
// PostgreSQLResult::Impl
// 持有 PQresult；逐行物化为字节表，避免暴露 libpq 类型。
// ---------------------------------------------------------------------------

struct PostgreSQLResult::Impl {
    PGresult* res = nullptr;
    std::vector<std::vector<std::vector<std::byte>>> rows;  // 已解码的 bytea/text 行
    std::size_t cursor = 0;
    std::size_t columnCountVal = 0;

    // 是否为 bytea 列（PQgetvalue 返回十六进制文本，需要解码）。
    std::vector<bool> isByteaCol;

    ~Impl() {
        if (res != nullptr) PQclear(res);
    }

    void materialize() {
        columnCountVal = static_cast<std::size_t>(PQnfields(res));
        isByteaCol.assign(columnCountVal, false);
        for (std::size_t c = 0; c < columnCountVal; ++c) {
            Oid type = PQftype(res, c);
            isByteaCol[c] = (type == kByteaOid);
        }
        const int n = PQntuples(res);
        rows.reserve(static_cast<std::size_t>(n));
        for (int r = 0; r < n; ++r) {
            std::vector<std::vector<std::byte>> row(columnCountVal);
            for (std::size_t c = 0; c < columnCountVal; ++c) {
                if (PQgetisnull(res, r, static_cast<int>(c))) continue;
                const char* v = PQgetvalue(res, r, static_cast<int>(c));
                std::size_t len = static_cast<std::size_t>(PQgetlength(res, r, static_cast<int>(c)));
                if (isByteaCol[c]) {
                    // bytea text 表示为 \x....，解码为原始字节
                    size_t decodedLen = 0;
                    if (unsigned char* decoded =
                            PQunescapeBytea(reinterpret_cast<const unsigned char*>(v), &decodedLen)) {
                        row[c].assign(reinterpret_cast<std::byte*>(decoded),
                                      reinterpret_cast<std::byte*>(decoded) + decodedLen);
                        PQfreemem(decoded);
                    }
                } else {
                    row[c].assign(reinterpret_cast<const std::byte*>(v),
                                  reinterpret_cast<const std::byte*>(v) + len);
                }
            }
            rows.push_back(std::move(row));
        }
    }
};

PostgreSQLResult::PostgreSQLResult() = default;

PostgreSQLResult::PostgreSQLResult(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

PostgreSQLResult::~PostgreSQLResult() = default;

PostgreSQLResult::PostgreSQLResult(PostgreSQLResult&&) noexcept = default;
PostgreSQLResult& PostgreSQLResult::operator=(PostgreSQLResult&&) noexcept = default;

bool PostgreSQLResult::next() {
    if (!impl_) return false;
    if (impl_->cursor >= impl_->rows.size()) return false;
    ++impl_->cursor;
    return true;
}

std::span<const std::byte> PostgreSQLResult::asBytes(std::size_t col) const {
    if (!impl_ || impl_->cursor == 0 || impl_->cursor > impl_->rows.size()) return {};
    const auto& row = impl_->rows[impl_->cursor - 1];
    if (col >= row.size()) return {};
    return {row[col].data(), row[col].size()};
}

std::string PostgreSQLResult::asString(std::size_t col) const {
    auto bytes = asBytes(col);
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::uint64_t PostgreSQLResult::asUint64(std::size_t col) const {
    return bytesToUint64(asBytes(col));
}

std::size_t PostgreSQLResult::columnCount() const {
    return impl_ ? impl_->columnCountVal : 0;
}

std::size_t PostgreSQLResult::rowCount() const {
    return impl_ ? impl_->rows.size() : 0;
}

// ---------------------------------------------------------------------------
// PostgreSQLConnection::Impl
// ---------------------------------------------------------------------------

struct PostgreSQLConnection::Impl {
    PGconn* conn = nullptr;
    mutable std::string lastError;
    std::uint64_t affectedRowsVal = 0;

    ~Impl() {
        if (conn != nullptr) PQfinish(conn);
    }

    void captureError() {
        if (conn != nullptr) {
            lastError = PQerrorMessage(conn);
        } else {
            lastError = "not connected";
        }
    }

    // 把 SqlParam 转成 libpq text 格式参数。所有字符串存活至 PQexecParams 返回。
    void buildParams(const std::vector<SqlParam>& params,
                     std::vector<const char*>& values,
                     std::vector<std::string>& storage) {
        storage.resize(params.size());
        values.resize(params.size(), nullptr);
        for (std::size_t i = 0; i < params.size(); ++i) {
            const auto& p = params[i];
            if (p.isNull) {
                values[i] = nullptr;  // libpq: nullptr 表示 SQL NULL
            } else if (p.isUint64) {
                storage[i] = u64ToDec(p.uint64Value);
                values[i] = storage[i].c_str();
            } else if (p.isString) {
                // 文本参数原样传递（不能走 ::bytea 十六进制，见 SqlParam::str）
                storage[i].assign(reinterpret_cast<const char*>(p.bytes.data()),
                                  p.bytes.size());
                values[i] = storage[i].c_str();
            } else {
                storage[i] = bytesToHex(p.bytes);
                values[i] = storage[i].c_str();
            }
        }
    }
};

PostgreSQLConnection::PostgreSQLConnection(PostgreSQLConnectionConfig config)
    : impl_(std::make_unique<Impl>()), config_(std::move(config)) {}

PostgreSQLConnection::~PostgreSQLConnection() = default;

bool PostgreSQLConnection::connect() {
    if (impl_->conn != nullptr) return true;

    std::ostringstream connInfo;
    connInfo << "host=" << config_.host << " port=" << config_.port
             << " user=" << config_.user << " dbname=" << config_.database
             << " connect_timeout=" << config_.connectTimeoutSeconds;
    if (!config_.password.empty()) {
        connInfo << " password=" << config_.password;
    }

    impl_->conn = PQconnectdb(connInfo.str().c_str());
    if (impl_->conn == nullptr || PQstatus(impl_->conn) != CONNECTION_OK) {
        impl_->captureError();
        if (impl_->conn != nullptr) {
            PQfinish(impl_->conn);
            impl_->conn = nullptr;
        }
        return false;
    }
    return true;
}

void PostgreSQLConnection::disconnect() {
    if (impl_->conn != nullptr) {
        PQfinish(impl_->conn);
        impl_->conn = nullptr;
    }
}

bool PostgreSQLConnection::isConnected() const {
    return impl_->conn != nullptr && PQstatus(impl_->conn) == CONNECTION_OK;
}

bool PostgreSQLConnection::execute(std::string_view sql, const std::vector<SqlParam>& params) {
    if (!ensureConnected()) return false;

    std::vector<const char*> values;
    std::vector<std::string> storage;
    impl_->buildParams(params, values, storage);

    PGresult* res = PQexecParams(impl_->conn, std::string(sql).c_str(),
                                 static_cast<int>(params.size()),
                                 nullptr,                 // 让服务器推断参数类型（配合 SQL 显式 cast）
                                 values.data(),
                                 nullptr,                 // text 格式无需长度
                                 nullptr,                 // 全部 text 格式
                                 0);                      // 文本结果
    if (res == nullptr) {
        impl_->captureError();
        return false;
    }
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        impl_->lastError = PQresultErrorMessage(res);
        PQclear(res);
        return false;
    }
    const char* tuples = PQcmdTuples(res);  // DML 返回影响行数字符串，DDL 为空
    impl_->affectedRowsVal =
        tuples[0] != '\0' ? static_cast<std::uint64_t>(std::strtoull(tuples, nullptr, 10)) : 0;
    PQclear(res);
    return true;
}

std::optional<PostgreSQLResult> PostgreSQLConnection::query(std::string_view sql,
                                                            const std::vector<SqlParam>& params) {
    if (!ensureConnected()) return std::nullopt;

    std::vector<const char*> values;
    std::vector<std::string> storage;
    impl_->buildParams(params, values, storage);

    PGresult* res = PQexecParams(impl_->conn, std::string(sql).c_str(),
                                 static_cast<int>(params.size()),
                                 nullptr, values.data(), nullptr, nullptr, 0);
    if (res == nullptr) {
        impl_->captureError();
        return std::nullopt;
    }
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        impl_->lastError = PQresultErrorMessage(res);
        PQclear(res);
        return std::nullopt;
    }

    auto impl = std::make_unique<PostgreSQLResult::Impl>();
    impl->res = res;
    impl->materialize();
    impl_->affectedRowsVal = impl->rows.size();
    return PostgreSQLResult(std::move(impl));
}

std::uint64_t PostgreSQLConnection::affectedRows() const {
    return impl_->affectedRowsVal;
}

bool PostgreSQLConnection::ping() {
    if (!isConnected()) return false;
    // 轻量探活：PQping 走新连接太重，直接跑一条空查询。
    PGresult* res = PQexec(impl_->conn, "SELECT 1");
    bool ok = res != nullptr && PQresultStatus(res) == PGRES_TUPLES_OK;
    if (res != nullptr) PQclear(res);
    return ok;
}

bool PostgreSQLConnection::ensureConnected() {
    if (isConnected()) return true;
    if (impl_->conn != nullptr) {
        PQreset(impl_->conn);  // 复用连接参数重连一次
        if (PQstatus(impl_->conn) == CONNECTION_OK) return true;
    }
    return connect();
}

const std::string& PostgreSQLConnection::lastError() const {
    return impl_->lastError;
}

}  // namespace theseed::db
