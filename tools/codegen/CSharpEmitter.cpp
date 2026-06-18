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

std::string deserializeCallForType(runtime::PropertyType type, const std::string& target) {
    switch (type) {
        case runtime::PropertyType::Int8:    return target + " = reader.ReadSByte()";
        case runtime::PropertyType::Int16:   return target + " = reader.ReadInt16()";
        case runtime::PropertyType::Int32:   return target + " = reader.ReadInt32()";
        case runtime::PropertyType::Int64:   return target + " = reader.ReadInt64()";
        case runtime::PropertyType::UInt8:   return target + " = reader.ReadByte()";
        case runtime::PropertyType::UInt16:  return target + " = reader.ReadUInt16()";
        case runtime::PropertyType::UInt32:  return target + " = reader.ReadUInt32()";
        case runtime::PropertyType::UInt64:  return target + " = reader.ReadUInt64()";
        case runtime::PropertyType::Float32: return target + " = reader.ReadSingle()";
        case runtime::PropertyType::Float64: return target + " = reader.ReadDouble()";
        case runtime::PropertyType::Bool:    return target + " = reader.ReadBoolean()";
        case runtime::PropertyType::String:  return target + " = reader.ReadString()";
        case runtime::PropertyType::Vector3: {
            std::ostringstream out;
            out << "        {\n"
                << "            float _x = reader.ReadSingle();\n"
                << "            float _y = reader.ReadSingle();\n"
                << "            float _z = reader.ReadSingle();\n"
                << "            " << target << " = new UnityEngine.Vector3(_x, _y, _z);\n"
                << "        }";
            return out.str();
        }
        case runtime::PropertyType::Blob:
            return target + " = reader.ReadBytes(reader.ReadInt32())";
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

    // Deserializer
    out << "        public override void Deserialize(BinaryReader reader)\n";
    out << "        {\n";
    out << "            base.Deserialize(reader);\n";
    for (const auto& prop : def.properties()) {
        std::string fieldName = pascalCase(prop.name);
        std::string target = isClientVisible(prop) ? (fieldName + ".current") : fieldName;
        std::string stmt = deserializeCallForType(prop.type, target);
        if (stmt.find('\n') != std::string::npos) {
            // 多行语句（如 Vector3），返回值本身已带正确缩进
            out << "            " << stmt << "\n";
            if (isClientVisible(prop)) {
                out << "            " << fieldName << ".NotifyChanged();\n";
            }
        } else {
            out << "            " << stmt << ";\n";
            if (isClientVisible(prop)) {
                out << "            " << fieldName << ".NotifyChanged();\n";
            }
        }
    }
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
