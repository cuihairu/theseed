#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace theseed::db {

// SQL 预处理语句绑定参数，二进制安全，MySQL / PostgreSQL 后端共用：
//   - 默认按字节串绑定（MySQL: MYSQL_TYPE_BLOB；PG: text 格式 + ::bytea 十六进制）
//   - str() 构造的文本参数按字符串绑定（MySQL 仍走 BLOB 绑定，行为不变；
//     PG 走原始 text——若按 bytea 十六进制绑进 VARCHAR 列，存进去的是
//     "\\x..." 字面文本，与 MySQL 版数据不一致）
//   - u64() 构造的整数按无符号 64 位整数绑定。
//     注意不要把整数的原始字节按字节串绑进 BIGINT 列——MySQL 严格模式会拒绝
//     （ERROR 1366 Incorrect integer value），PG 会把它当 bytea/text 误解。
struct SqlParam {
    SqlParam() = default;
    SqlParam(std::vector<std::byte> data)  // 允许从字节串隐式转换
        : bytes(std::move(data)) {}

    // 无符号 64 位整数参数（如 EntityId）。
    static SqlParam u64(std::uint64_t value) {
        SqlParam p;
        p.isUint64 = true;
        p.uint64Value = value;
        return p;
    }

    // 文本参数（用户名等绑定到字符类型的列）。不得内含 NUL 字节。
    static SqlParam str(std::string value) {
        SqlParam p;
        p.isString = true;
        p.bytes.assign(reinterpret_cast<const std::byte*>(value.data()),
                       reinterpret_cast<const std::byte*>(value.data()) + value.size());
        return p;
    }

    // SQL NULL 参数。
    static SqlParam null() {
        SqlParam p;
        p.isNull = true;
        return p;
    }

    bool isNull = false;
    bool isUint64 = false;
    bool isString = false;
    std::uint64_t uint64Value = 0;
    std::vector<std::byte> bytes;
};

}  // namespace theseed::db
