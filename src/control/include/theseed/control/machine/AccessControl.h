#pragma once

#include "theseed/runtime/RuntimeTypes.h"

#include <cstdint>
#include <vector>

namespace theseed::control::machine {

// 04-ops-control-plane §6.1 权限分级（落地形态：来源组件 → 角色的绑定表
// + 每个动作族所需的最低角色）。分级单调：Admin 覆盖 Operator 的动作，
// Operator 覆盖 ReadOnly 的动作，None（未绑定）一律拒绝——绑定表缺失
// 的组件没有任何动作可用（安全缺省：未显式授权即不可用）。
enum class AccessRole : std::uint8_t {
    None = 0,   // 未绑定：全拒
    ReadOnly,   // 只读面：inspect / snapshot / audit / 剖面查询与下载
    Operator,   // 操作面：受管进程编排（execute start/stop/restart）、
                // 诊断采样触发（§6.1 的 kick session / set draining /
                // clear temporary bans 同级，命令本身仓库未建，见边界）
    Admin,      // 管理面：主机进程处置（machine.terminate ≙ §6.1 的
                // retire process）、运行时配置生效（§6.3）
};

// 分级判定：role 达到 required 的档位即可用该动作。
constexpr bool roleMeets(AccessRole role, AccessRole required) {
    return static_cast<std::uint8_t>(role) >= static_cast<std::uint8_t>(required);
}

// 角色绑定（04 §6.2 的 operatorId 口径沿用既有 source 组件 id——审计
// 条目的 source 即操作者身份，不另设操作者命名空间）。
struct RoleBinding final {
    runtime::ComponentId component = 0;
    AccessRole role = AccessRole::None;
};

// 绑定表查询：线性扫描（绑定表与既有策略白名单同量级，个位数条目）；
// 未绑定返回 None。
inline AccessRole roleFor(const std::vector<RoleBinding>& bindings,
                          runtime::ComponentId component) {
    for (const auto& binding : bindings) {
        if (binding.component == component) {
            return binding.role;
        }
    }
    return AccessRole::None;
}

}  // namespace theseed::control::machine
