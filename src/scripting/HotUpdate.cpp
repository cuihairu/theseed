#include "theseed/scripting/HotUpdate.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace theseed::scripting {

HotUpdateLevel levelOf(ChangeType type) noexcept {
    switch (type) {
        case ChangeType::ConfigValueChange:
            return HotUpdateLevel::L1_Config;
        case ChangeType::ModifyScriptBody:
        case ChangeType::ModifyTimerLogic:
            return HotUpdateLevel::L2_Script;
        case ChangeType::AddPropertyDefault:
        case ChangeType::AddEntityType:
            return HotUpdateLevel::L3_Def;
        case ChangeType::ChangePropertyType:
        case ChangeType::RemoveProperty:
        case ChangeType::ChangeMethodSignature:
        case ChangeType::ChangeExposedProtocol:
        case ChangeType::ChangeSerialization:
            return HotUpdateLevel::L4_NeedRestart;
        default:
            return HotUpdateLevel::L4_NeedRestart;
    }
}

bool isAutoApprove(ChangeType type) noexcept {
    return type == ChangeType::ConfigValueChange ||
           type == ChangeType::ModifyScriptBody;
}

bool isForbidden(ChangeType type) noexcept {
    return levelOf(type) == HotUpdateLevel::L4_NeedRestart;
}

namespace {

std::string describeChange(const DiffChange& c) {
    std::ostringstream out;
    if (!c.entityName.empty()) out << "[" << c.entityName << "] ";
    if (!c.key.empty()) out << c.key << ": ";
    if (!c.detail.empty()) out << c.detail;
    return out.str();
}

}  // namespace

ValidationResult HotUpdateValidator::validate(const DiffResult& diff, PolicyMode /*mode*/) const {
    ValidationResult result;
    result.level = HotUpdateLevel::L1_Config;  // 空差异也允许热更（no-op）

    HotUpdateLevel maxLevel = HotUpdateLevel::L1_Config;

    for (const auto& change : diff.changes) {
        const auto lvl = levelOf(change.type);

        if (lvl == HotUpdateLevel::L4_NeedRestart) {
            std::ostringstream msg;
            msg << "forbidden change (" << static_cast<int>(change.type)
                << "): " << describeChange(change);
            result.rejections.push_back(msg.str());
            maxLevel = HotUpdateLevel::L4_NeedRestart;
            continue;
        }
        if (lvl == HotUpdateLevel::L3_Def) {
            std::ostringstream msg;
            msg << "def-level change requires Phase 2 schema migration ("
                << static_cast<int>(change.type) << "): " << describeChange(change);
            result.rejections.push_back(msg.str());
            if (maxLevel < HotUpdateLevel::L4_NeedRestart) {
                maxLevel = HotUpdateLevel::L4_NeedRestart;
            }
            continue;
        }
        if (change.type == ChangeType::ModifyTimerLogic) {
            std::ostringstream msg;
            msg << "manual review recommended for timer logic change: "
                << describeChange(change);
            result.warnings.push_back(msg.str());
        }
        if (lvl > maxLevel) maxLevel = lvl;
    }

    result.level = maxLevel;
    return result;
}

HotUpdateManager::HotUpdateManager() {
    history_.emplace_back();
    history_.front().first = "v0";  // 初始版本
    currentVersion_ = "v0";
}

const std::unordered_map<std::string, std::string>& HotUpdateManager::configStore() const noexcept {
    return configStore_;
}

const std::unordered_map<std::string, std::string>& HotUpdateManager::scriptStore() const noexcept {
    return scriptStore_;
}

std::vector<std::string> HotUpdateManager::appliedVersions() const noexcept {
    std::vector<std::string> versions;
    versions.reserve(history_.size());
    for (const auto& [v, _] : history_) versions.push_back(v);
    return versions;
}

std::string HotUpdateManager::currentVersion() const noexcept {
    return currentVersion_;
}

void HotUpdateManager::applyChange(const DiffChange& change) {
    switch (change.type) {
        case ChangeType::ConfigValueChange:
            configStore_[change.key] = change.detail;
            break;
        case ChangeType::ModifyScriptBody:
        case ChangeType::ModifyTimerLogic:
            // L2 范围内：以 key（方法名）为索引，detail 视为新方法体
            scriptStore_[change.key] = change.detail;
            break;
        default:
            // validator 已拦截 L3/L4，这里不应到达
            throw std::logic_error("HotUpdateManager::applyChange reached forbidden change type");
    }
}

HotUpdateResult HotUpdateManager::apply(const DiffResult& diff) {
    HotUpdateValidator validator;
    auto verdict = validator.validate(diff);
    if (!verdict.canHotUpdate()) {
        std::ostringstream msg;
        msg << "rejected " << verdict.rejections.size() << " change(s); level="
            << static_cast<int>(verdict.level);
        HotUpdateResult result;
        result.message = msg.str();
        return result;
    }

    // 应用变更到 configStore_ / scriptStore_
    for (const auto& change : diff.changes) {
        applyChange(change);
    }

    // 保存"应用后状态"快照（rollback 回到这个版本时使用）
    Snapshot snap;
    snap.config = configStore_;
    snap.scripts = scriptStore_;

    std::ostringstream vn;
    vn << "v" << history_.size();
    const auto newVersion = vn.str();
    history_.emplace_back(newVersion, std::move(snap));
    currentVersion_ = newVersion;

    HotUpdateResult result;
    result.success = true;
    result.appliedVersion = newVersion;
    std::ostringstream msg;
    msg << "applied " << diff.changes.size() << " change(s) at " << newVersion;
    if (!verdict.warnings.empty()) {
        msg << " (" << verdict.warnings.size() << " warning(s))";
    }
    result.message = msg.str();
    return result;
}

HotUpdateResult HotUpdateManager::rollback(const std::string& version) {
    if (history_.size() <= 1) {
        HotUpdateResult r;
        r.message = "no history to roll back to";
        return r;
    }

    std::vector<std::pair<std::string, Snapshot>>::const_iterator target;
    if (version.empty()) {
        // 回滚到上一个版本
        target = history_.end() - 2;
    } else {
        target = std::find_if(history_.begin(), history_.end() - 1,
            [&](const auto& entry) { return entry.first == version; });
        if (target == history_.end() - 1) {
            HotUpdateResult r;
            r.message = "version not found: " + version;
            return r;
        }
    }

    configStore_ = target->second.config;
    scriptStore_ = target->second.scripts;
    currentVersion_ = target->first;
    // 保留历史但截断到 target 之后；后续 apply 会创建新版本号
    history_.erase(target + 1, history_.end());

    HotUpdateResult r;
    r.success = true;
    r.appliedVersion = currentVersion_;
    std::ostringstream msg;
    msg << "rolled back to " << currentVersion_;
    r.message = msg.str();
    return r;
}

}  // namespace theseed::scripting
