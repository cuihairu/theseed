#pragma once

#include "theseed/core/EntityData.h"

#include <cstdint>
#include <string>

namespace theseed::db {

// 账号查询接口。Account 实体的 username/password 查询是登录链路的热路径，
// 不同于通用的 EntityData load——后者需要反序列化全部属性，而登录只需
// 按 username 索引命中并取出密码。MySQL 后端用专用索引表实现，
// FileEntityStore 后端保持线性扫描的回退实现（在 DBApp 内）。
class IAccountStore {
public:
    virtual ~IAccountStore() = default;

    // 按 username 查找账号。命中返回 true 并填充 entityId 与 password。
    virtual bool queryAccount(const std::string& username,
                              core::EntityId& outId,
                              std::string& outPassword) = 0;

    // 创建账号。用户名已存在返回 false；创建成功返回 true 并填充新 entityId。
    virtual bool createAccount(const std::string& username,
                               const std::string& password,
                               core::EntityId& outId) = 0;
};

}  // namespace theseed::db
