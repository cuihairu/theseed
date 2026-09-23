#include "theseed/codegen/CSharpEmitter.h"

#include <cctype>
#include <sstream>
#include <stdexcept>

namespace theseed::codegen {

namespace {

std::string pascalCase(const std::string& name) {
    if (name.empty()) return name;
    std::string out;
    out.reserve(name.size() + 1);
    bool capitalizeNext = true;
    for (char c : name) {
        if (c == '_' || c == ' ') {
            capitalizeNext = true;
            continue;
        }
        out.push_back(capitalizeNext ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
        capitalizeNext = false;
    }
    return out;
}

bool isClientVisible(const runtime::PropertyDescriptor& prop) {
    using runtime::PropertyFlag;
    return hasFlag(prop.flags, PropertyFlag::ClientSync);
}

bool isExposedMethod(const runtime::MethodDescriptor& method) {
    // MVP 简化：side==Base 或 side==Cell 的方法均可被客户端调用（exposed）。
    // side==Client 是服务器→客户端的回调，生成 partial 桩。
    return method.side == runtime::MethodSide::Base ||
           method.side == runtime::MethodSide::Cell;
}

std::string methodArgSignature(const runtime::MethodDescriptor& method) {
    std::ostringstream out;
    bool first = true;
    int idx = 0;
    for (const auto& arg : method.args) {
        if (!first) out << ", ";
        first = false;
        std::string argName = arg.name.empty() ? ("arg" + std::to_string(idx)) : arg.name;
        out << CSharpEmitter::csharpType(arg.type) << " " << argName;
        ++idx;
    }
    return out.str();
}

std::string methodArgCallList(const runtime::MethodDescriptor& method) {
    std::ostringstream out;
    bool first = true;
    int idx = 0;
    for (const auto& arg : method.args) {
        if (!first) out << ", ";
        first = false;
        std::string argName = arg.name.empty() ? ("arg" + std::to_string(idx)) : arg.name;
        out << argName;
        ++idx;
    }
    return out.str();
}

std::string serializeCallForType(runtime::PropertyType type, const std::string& accessor) {
    switch (type) {
        case runtime::PropertyType::Int8:    return "writer.Write((sbyte)" + accessor + ")";
        case runtime::PropertyType::Int16:   return "writer.Write((short)" + accessor + ")";
        case runtime::PropertyType::Int32:   return "writer.Write(" + accessor + ")";
        case runtime::PropertyType::Int64:   return "writer.Write((long)" + accessor + ")";
        case runtime::PropertyType::UInt8:   return "writer.Write((byte)" + accessor + ")";
        case runtime::PropertyType::UInt16:  return "writer.Write((ushort)" + accessor + ")";
        case runtime::PropertyType::UInt32:  return "writer.Write((uint)" + accessor + ")";
        case runtime::PropertyType::UInt64:  return "writer.Write((ulong)" + accessor + ")";
        case runtime::PropertyType::Float32: return "writer.Write(" + accessor + ")";
        case runtime::PropertyType::Float64: return "writer.Write((double)" + accessor + ")";
        case runtime::PropertyType::Bool:    return "writer.Write(" + accessor + ")";
        case runtime::PropertyType::String:  return "writer.Write(" + accessor + ")";
        case runtime::PropertyType::Vector3: return "writer.Write(" + accessor + ".x); writer.Write(" + accessor + ".y); writer.Write(" + accessor + ".z)";
        case runtime::PropertyType::Blob:    return "writer.Write(" + accessor + ")";
        default:
            throw std::invalid_argument("Unsupported property type in serializer");
    }
}

std::string deserializeReadExpr(runtime::PropertyType type) {
    switch (type) {
        case runtime::PropertyType::Int8:    return "reader.ReadSByte()";
        case runtime::PropertyType::Int16:   return "reader.ReadInt16()";
        case runtime::PropertyType::Int32:   return "reader.ReadInt32()";
        case runtime::PropertyType::Int64:   return "reader.ReadInt64()";
        case runtime::PropertyType::UInt8:   return "reader.ReadByte()";
        case runtime::PropertyType::UInt16:  return "reader.ReadUInt16()";
        case runtime::PropertyType::UInt32:  return "reader.ReadUInt32()";
        case runtime::PropertyType::UInt64:  return "reader.ReadUInt64()";
        case runtime::PropertyType::Float32: return "reader.ReadSingle()";
        case runtime::PropertyType::Float64: return "reader.ReadDouble()";
        case runtime::PropertyType::Bool:    return "reader.ReadBoolean()";
        case runtime::PropertyType::String:  return "reader.ReadString()";
        // Vector3 / Blob 在 emitEntity 中单独处理（多语句 / 已知长度读取），
        // 不走单值表达式路径。
        case runtime::PropertyType::Vector3:
        case runtime::PropertyType::Blob:
        default:
            throw std::invalid_argument("Unsupported property type in deserializer");
    }
}

}  // namespace

CSharpEmitter::CSharpEmitter(CSharpEmitOptions options)
    : options_(std::move(options)) {}

std::string CSharpEmitter::csharpType(runtime::PropertyType type) {
    switch (type) {
        case runtime::PropertyType::Int8:    return "sbyte";
        case runtime::PropertyType::Int16:   return "short";
        case runtime::PropertyType::Int32:   return "int";
        case runtime::PropertyType::Int64:   return "long";
        case runtime::PropertyType::UInt8:   return "byte";
        case runtime::PropertyType::UInt16:  return "ushort";
        case runtime::PropertyType::UInt32:  return "uint";
        case runtime::PropertyType::UInt64:  return "ulong";
        case runtime::PropertyType::Float32: return "float";
        case runtime::PropertyType::Float64: return "double";
        case runtime::PropertyType::Bool:    return "bool";
        case runtime::PropertyType::String:  return "string";
        case runtime::PropertyType::Vector3: return "UnityEngine.Vector3";
        case runtime::PropertyType::Blob:    return "byte[]";
        default:
            throw std::invalid_argument("Unsupported property type");
    }
}

CSharpEmitter::OutputFiles CSharpEmitter::emit(
    const std::vector<const runtime::EntityDef*>& defs) const {
    OutputFiles files;
    for (const auto* def : defs) {
        if (!def) continue;
        auto [name, content] = emitEntity(*def);
        files[std::move(name)] = std::move(content);
    }
    if (!defs.empty()) {
        auto [name, content] = emitRegistry(defs);
        files[std::move(name)] = std::move(content);
    }
    return files;
}

std::pair<std::string, std::string> CSharpEmitter::emitEntity(const runtime::EntityDef& def) const {
    const auto className = pascalCase(def.entityType());
    std::ostringstream out;

    if (options_.emitHeader) {
        out << "// <auto-generated>\n";
        out << "//     Generated by theseed-codegen from entity def \"" << def.entityType() << "\".\n";
        out << "//     Do not edit by hand; re-run codegen to regenerate.\n";
        out << "// </auto-generated>\n\n";
    }

    out << "using System.IO;\n";
    out << "using Theseed.Runtime;\n\n";
    out << "namespace " << options_.namespaceName << "\n";
    out << "{\n";
    out << "    public sealed partial class " << className << " : EntityBase\n";
    out << "    {\n";

    // Properties
    for (const auto& prop : def.properties()) {
        std::string fieldName = pascalCase(prop.name);
        if (isClientVisible(prop)) {
            out << "        public SyncProperty<" << csharpType(prop.type) << "> " << fieldName
                << " { get; } = new SyncProperty<" << csharpType(prop.type) << ">(default);\n";
        } else {
            out << "        public " << csharpType(prop.type) << " " << fieldName << " { get; set; }\n";
        }
    }
    if (!def.properties().empty()) out << "\n";

    // Methods
    for (const auto& method : def.methods()) {
        std::string methodName = pascalCase(method.name);
        if (isExposedMethod(method)) {
            // Client → Server
            out << "        public void Call_" << methodName
                << "(" << methodArgSignature(method) << ")\n";
            out << "        {\n";
            out << "            InvokeServerMethod(" << method.id
                << (methodArgCallList(method).empty() ? "" : ", ")
                << methodArgCallList(method) << ");\n";
            out << "        }\n\n";
        } else {
            // Server → Client (callback stub)
            out << "        partial void On_" << methodName
                << "(" << methodArgSignature(method) << ");\n\n";
        }
    }

    // Serializer
    out << "        public override void Serialize(BinaryWriter writer)\n";
    out << "        {\n";
    out << "            base.Serialize(writer);\n";
    for (const auto& prop : def.properties()) {
        std::string fieldName = pascalCase(prop.name);
        std::string accessor = isClientVisible(prop) ? fieldName + ".current" : fieldName;
        out << "            " << serializeCallForType(prop.type, accessor) << ";\n";
    }
    out << "        }\n\n";

    // Deserializer —— 解析服务端 keyed-delta 流。
    // 服务端格式（PropertyReplication::encodeDelta）：
    //   [u32 count][repeat: u32 propertyId, u32 valueSize, value bytes]
    // 客户端按 propertyId 路由到对应字段，未知属性跳过（向前兼容）。
    out << "        public override void Deserialize(BinaryReader reader)\n";
    out << "        {\n";
    out << "            base.Deserialize(reader);\n";
    out << "            int count = reader.ReadInt32();\n";
    out << "            for (int i = 0; i < count; i++)\n";
    out << "            {\n";
    out << "                uint propId = reader.ReadUInt32();\n";
    out << "                int len = reader.ReadInt32();\n";
    out << "                switch (propId)\n";
    out << "                {\n";
    for (const auto& prop : def.properties()) {
        std::string fieldName = pascalCase(prop.name);
        std::string target = isClientVisible(prop) ? (fieldName + ".current") : fieldName;
        out << "                    case " << prop.id << ": // " << prop.name << "\n";
        // Blob 读取需要长度前缀，这里 len 已由 delta 头给出，直接 ReadBytes(len)。
        if (prop.type == runtime::PropertyType::Blob) {
            out << "                        " << fieldName << " = reader.ReadBytes(len);\n";
        } else if (prop.type == runtime::PropertyType::Vector3) {
            out << "                        {\n";
            out << "                            float _x = reader.ReadSingle();\n";
            out << "                            float _y = reader.ReadSingle();\n";
            out << "                            float _z = reader.ReadSingle();\n";
            out << "                            " << target << " = new UnityEngine.Vector3(_x, _y, _z);\n";
            out << "                        }\n";
        } else {
            // 单值读取：deserializeValueExpr(type) 返回 reader.ReadXxx()
            out << "                        " << target << " = "
                << deserializeReadExpr(prop.type) << ";\n";
        }
        if (isClientVisible(prop)) {
            out << "                        " << fieldName << ".NotifyChanged();\n";
        }
        out << "                        break;\n";
    }
    out << "                    default:\n";
    out << "                        reader.ReadBytes(len); // 跳过未知属性\n";
    out << "                        break;\n";
    out << "                }\n";
    out << "            }\n";
    out << "        }\n";

    out << "    }\n";
    out << "}\n";

    return { className + ".Generated.cs", out.str() };
}

std::pair<std::string, std::string> CSharpEmitter::emitRegistry(
    const std::vector<const runtime::EntityDef*>& defs) const {
    std::ostringstream out;

    if (options_.emitHeader) {
        out << "// <auto-generated>\n";
        out << "//     Generated by theseed-codegen.\n";
        out << "//     Lists all entity types known to the runtime.\n";
        out << "// </auto-generated>\n\n";
    }

    out << "namespace " << options_.namespaceName << "\n";
    out << "{\n";
    out << "    public static class EntityRegistry\n";
    out << "    {\n";
    out << "        public const int EntityCount = " << defs.size() << ";\n\n";
    out << "        public static readonly string[] EntityNames = new string[]\n";
    out << "        {\n";
    for (std::size_t i = 0; i < defs.size(); ++i) {
        out << "            \"" << defs[i]->entityType() << "\"";
        if (i + 1 < defs.size()) out << ",";
        out << "\n";
    }
    out << "        };\n";
    out << "    }\n";
    out << "}\n";

    return { "EntityRegistry.Generated.cs", out.str() };
}

}  // namespace theseed::codegen
