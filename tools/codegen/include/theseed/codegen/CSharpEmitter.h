#pragma once

#include "theseed/runtime/EntityDef.h"

#include <map>
#include <string>
#include <vector>

namespace theseed::codegen {

// MVP Phase B-4: 离线 EntityDef → C# (Unity) 代码生成器。
//
// 输入是一组 EntityDef（已经通过 EntityDefLoader 从 XML 加载），
// 输出是一组 (文件名, 文件内容) 对，由 main.cpp 落盘到 --output-unity 目录。
//
// 生成范围（与 docs/design/0-foundation/01-mvp-architecture-baseline.md
//             第 11 章第 438–442 行对齐）：
//   - Entity 定义（partial class）
//   - Exposed 方法桩（CallServer 调用 + partial 回调）
//   - 基础序列化代码（Serialize/Deserialize）
//
// 不生成（Phase 2 范围）：
//   - NetworkClient / EntitySyncEngine 运行时 SDK
//   - Blueprint 深度集成
//   - 预测回滚框架

struct CSharpEmitOptions final {
    // 输出的 C# 命名空间。默认 "Theseed.Generated"。
    std::string namespaceName = "Theseed.Generated";
    // 是否生成注释头（生成时间 / 来源）。测试时关闭便于断言。
    bool emitHeader = true;
};

class CSharpEmitter final {
public:
    using OutputFiles = std::map<std::string, std::string>;

    explicit CSharpEmitter(CSharpEmitOptions options = {});

    // 为每个 EntityDef 生成一个 .cs 文件，并额外生成一份
    // EntityRegistry.Generated.cs 集中注册表。
    OutputFiles emit(const std::vector<const runtime::EntityDef*>& defs) const;

    // 单个实体的生成入口。返回文件名（不含路径）与文件内容。
    // 暴露给测试，避免在测试里组装 OutputFiles。
    std::pair<std::string, std::string> emitEntity(const runtime::EntityDef& def) const;

    // 生成集中注册表（EntityType manifest）。
    std::pair<std::string, std::string> emitRegistry(
        const std::vector<const runtime::EntityDef*>& defs) const;

    // 类型映射：PropertyType → C# 类型名（公开以便测试）。
    static std::string csharpType(runtime::PropertyType type);

private:
    CSharpEmitOptions options_;
};

}  // namespace theseed::codegen
