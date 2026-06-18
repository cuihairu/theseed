#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace theseed::scripting {

// MVP Phase C-5: 受限脚本热更（L1 配置 + L2 脚本实现替换）。
//
// 与 docs/design/7-scripting-and-client/02-hot-update.md §0 / §1 / §3 / §4.1 对齐。
// MVP 不实现 L3/L4；只要变更越过 §2 的 L2 边界，必须升级为 L4_NeedRestart。
//
// 本模块只做：
//   - 差异分类（ChangeType → HotUpdateLevel）
//   - validator 把 §4.1 forbidden 变更识别为 L4
//   - manager 在 L1/L2 范围内原子应用，并提供版本快照支持 rollback
//
// 不做（Phase 2 范围）：
//   - 真实脚本 VM 绑定（Python/Lua 接入）
//   - 在线协程无缝续跑
//   - 跨版本协议兼容

enum class HotUpdateLevel : std::uint8_t {
    L1_Config = 0,         // 配置热更：自动放行
    L2_Script = 1,         // 脚本热更：方法体 / 定时器逻辑
    L3_Def = 2,            // 定义热更：新增属性 / 新增 Entity（Phase 2）
    L4_NeedRestart = 3,    // 结构变更：必须 rolling update
};

enum class ChangeType : std::uint8_t {
    // L1
    ConfigValueChange = 0,
    // L2 auto-approve
    ModifyScriptBody,
    // L2 manual review
    ModifyTimerLogic,
    // L3
    AddPropertyDefault,
    AddEntityType,
    // L4 forbidden
    ChangePropertyType,
    RemoveProperty,
    ChangeMethodSignature,
    ChangeExposedProtocol,
    ChangeSerialization,
};

struct DiffChange {
    ChangeType type = ChangeType::ConfigValueChange;
    std::string entityName;   // 可选，用于错误信息
    std::string key;          // 配置键 / 脚本方法名 / 属性名
    std::string detail;       // 人类可读说明
};

struct DiffResult {
    std::vector<DiffChange> changes;
};

struct ValidationResult {
    HotUpdateLevel level = HotUpdateLevel::L1_Config;
    std::vector<std::string> rejections;   // 阻断理由（L4 必填）
    std::vector<std::string> warnings;     // 人工审核建议（L2 manual review）
    bool applied = false;                  // validate 不修改 applied，仅用于回显

    bool canHotUpdate() const noexcept {
        return level == HotUpdateLevel::L1_Config || level == HotUpdateLevel::L2_Script;
    }
};

struct HotUpdateResult {
    bool success = false;
    std::string message;
    std::string appliedVersion;            // apply 成功后的版本号
};

// 将单个 ChangeType 映射到它所属的 HotUpdateLevel。
// 公开以便测试。
HotUpdateLevel levelOf(ChangeType type) noexcept;

// 是否属于 §4.1 "自动放行候选"。
bool isAutoApprove(ChangeType type) noexcept;

// 是否属于 §4.1 "自动阻断"。
bool isForbidden(ChangeType type) noexcept;

class HotUpdateValidator final {
public:
    enum class PolicyMode : std::uint8_t {
        Strict,           // ModifyTimerLogic 视为需要人工审核（warnings 中提示）
        AllowManualReview, // 与 Strict 等价；保留枚举以便 Phase 2 引入更多策略
    };

    // 分析整份 DiffResult，输出最终 HotUpdateLevel 与 rejections/warnings。
    // 命中任一 forbidden → level = L4。
    // 全部 auto-approve → level = L1 或 L2（L2 优先于 L1）。
    // ModifyTimerLogic 不影响 level（仍在 L2 范围），但会写入 warnings。
    ValidationResult validate(const DiffResult& diff,
                              PolicyMode mode = PolicyMode::Strict) const;
};

// 简化的快照式 Manager：L1 配置 + L2 脚本方法体的原子应用 + rollback。
// Phase 2 引入真实 VM 后，applyScriptBody 内部替换为 VM reload。
class HotUpdateManager final {
public:
    HotUpdateManager();

    // 1. 内部调用 validator.validate(diff)
    // 2. canHotUpdate==false → 直接返回失败
    // 3. canHotUpdate==true → 应用 L1 / L2 变更，递增版本号
    HotUpdateResult apply(const DiffResult& diff);

    // 回滚到指定版本（必须由 apply 产生）。返回到旧快照。
    // version 为空时回滚到上一个版本。
    HotUpdateResult rollback(const std::string& version = {});

    // 诊断接口（测试用）
    const std::unordered_map<std::string, std::string>& configStore() const noexcept;
    const std::unordered_map<std::string, std::string>& scriptStore() const noexcept;
    std::vector<std::string> appliedVersions() const noexcept;
    std::string currentVersion() const noexcept;

private:
    struct Snapshot {
        std::unordered_map<std::string, std::string> config;
        std::unordered_map<std::string, std::string> scripts;
    };

    void applyChange(const DiffChange& change);

    std::unordered_map<std::string, std::string> configStore_;
    std::unordered_map<std::string, std::string> scriptStore_;
    std::vector<std::pair<std::string, Snapshot>> history_;  // version → snapshot
    std::string currentVersion_;
};

}  // namespace theseed::scripting
