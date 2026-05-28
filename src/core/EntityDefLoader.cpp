#include "theseed/core/EntityDefLoader.h"
#include "theseed/runtime/EntityDef.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace theseed::core {

namespace {

struct XmlAttr {
    std::string key;
    std::string value;
};

struct XmlNode {
    std::string tag;
    std::vector<XmlAttr> attrs;
    std::vector<XmlNode> children;
};

std::string_view skipWs(std::string_view sv) {
    while (!sv.empty() && (sv[0] == ' ' || sv[0] == '\t' || sv[0] == '\n' || sv[0] == '\r')) {
        sv.remove_prefix(1);
    }
    return sv;
}

std::string_view parseAttrs(std::string_view sv, std::vector<XmlAttr>& attrs) {
    while (!sv.empty()) {
        sv = skipWs(sv);
        if (sv.empty() || sv[0] == '>' || sv[0] == '/') break;

        std::string key;
        while (!sv.empty() && sv[0] != '=' && sv[0] != ' ' && sv[0] != '>' && sv[0] != '/' && sv[0] != '\t' && sv[0] != '\n') {
            key += sv[0];
            sv.remove_prefix(1);
        }

        sv = skipWs(sv);
        if (sv.empty() || sv[0] != '=') break;
        sv.remove_prefix(1);
        sv = skipWs(sv);

        char quote = '"';
        if (!sv.empty() && (sv[0] == '"' || sv[0] == '\'')) {
            quote = sv[0];
            sv.remove_prefix(1);
        }

        std::string value;
        while (!sv.empty() && sv[0] != quote) {
            value += sv[0];
            sv.remove_prefix(1);
        }
        if (!sv.empty()) sv.remove_prefix(1);

        if (!key.empty()) {
            attrs.push_back({std::move(key), std::move(value)});
        }
    }
    return sv;
}

// Returns remaining string_view after parsing children until the closing </tag>
std::string_view parseChildren(std::string_view sv, const std::string& parentTag, std::vector<XmlNode>& children) {
    while (!sv.empty()) {
        sv = skipWs(sv);
        if (sv.empty()) break;

        // Look for closing tag
        if (sv.starts_with("</")) {
            auto closeEnd = sv.find('>');
            if (closeEnd == std::string_view::npos) break;
            // Verify it matches parent tag
            auto closingTag = sv.substr(2, closeEnd - 2);
            auto trimmed = skipWs(closingTag);
            // Return past the closing tag
            return sv.substr(closeEnd + 1);
        }

        // Skip XML declaration
        if (sv.starts_with("<?")) {
            auto end = sv.find("?>");
            if (end == std::string_view::npos) break;
            sv.remove_prefix(end + 2);
            continue;
        }

        // Skip comments
        if (sv.starts_with("<!--")) {
            auto end = sv.find("-->");
            if (end == std::string_view::npos) break;
            sv.remove_prefix(end + 3);
            continue;
        }

        // Parse opening tag
        if (sv[0] != '<') break;
        sv.remove_prefix(1);

        sv = skipWs(sv);
        if (sv.empty()) break;

        XmlNode node;
        while (!sv.empty() && sv[0] != ' ' && sv[0] != '>' && sv[0] != '/' && sv[0] != '\t' && sv[0] != '\n' && sv[0] != '\r') {
            node.tag += sv[0];
            sv.remove_prefix(1);
        }

        sv = parseAttrs(sv, node.attrs);
        sv = skipWs(sv);
        if (sv.empty()) break;

        // Self-closing tag
        if (sv[0] == '/') {
            sv.remove_prefix(1);
            sv = skipWs(sv);
            if (!sv.empty() && sv[0] == '>') sv.remove_prefix(1);
            children.push_back(std::move(node));
            continue;
        }

        // Regular opening tag, skip '>'
        if (sv[0] != '>') break;
        sv.remove_prefix(1);

        // Recursively parse children until our closing tag
        sv = parseChildren(sv, node.tag, node.children);
        children.push_back(std::move(node));
    }

    return sv;
}

bool parseXml(std::string_view sv, XmlNode& root) {
    root = XmlNode{};
    parseChildren(sv, "", root.children);
    return !root.children.empty();
}

std::string findAttr(const XmlNode& node, const std::string& key, const std::string& defaultVal = {}) {
    for (const auto& attr : node.attrs) {
        if (attr.key == key) return attr.value;
    }
    return defaultVal;
}

runtime::PropertyType parsePropertyType(const std::string& typeStr) {
    if (typeStr == "Int8")    return runtime::PropertyType::Int8;
    if (typeStr == "Int16")   return runtime::PropertyType::Int16;
    if (typeStr == "Int32")   return runtime::PropertyType::Int32;
    if (typeStr == "Int64")   return runtime::PropertyType::Int64;
    if (typeStr == "UInt8")   return runtime::PropertyType::UInt8;
    if (typeStr == "UInt16")  return runtime::PropertyType::UInt16;
    if (typeStr == "UInt32")  return runtime::PropertyType::UInt32;
    if (typeStr == "UInt64")  return runtime::PropertyType::UInt64;
    if (typeStr == "Float32") return runtime::PropertyType::Float32;
    if (typeStr == "Float64") return runtime::PropertyType::Float64;
    if (typeStr == "Bool")    return runtime::PropertyType::Bool;
    if (typeStr == "String")  return runtime::PropertyType::String;
    if (typeStr == "Vector3") return runtime::PropertyType::Vector3;
    if (typeStr == "Blob")    return runtime::PropertyType::Blob;
    throw std::runtime_error("Unknown property type: " + typeStr);
}

}  // anonymous namespace

std::unique_ptr<EntityDef> EntityDefLoader::loadFromString(const std::string& xml) {
    XmlNode root;
    if (!parseXml(xml, root) || root.children.empty()) {
        throw std::runtime_error("Failed to parse entity definition XML");
    }

    auto& entityNode = root.children[0];
    if (entityNode.tag != "EntityDef") {
        throw std::runtime_error("Expected root tag <EntityDef>");
    }

    auto entityName = findAttr(entityNode, "name");
    auto def = std::make_unique<EntityDef>(entityName);

    auto extends = findAttr(entityNode, "extends");
    if (!extends.empty()) {
        def->setParentType(std::move(extends));
    }

    for (auto& child : entityNode.children) {
        if (child.tag == "Properties") {
            for (auto& propNode : child.children) {
                if (propNode.tag != "Property") continue;

                auto name = findAttr(propNode, "name");
                auto typeStr = findAttr(propNode, "type", "Int32");

                if (name.empty()) {
                    throw std::runtime_error("Property missing 'name' attribute");
                }

                auto type = parsePropertyType(typeStr);

                runtime::PropertyFlag flags = runtime::PropertyFlag::None;
                auto flagStr = findAttr(propNode, "flags");
                if (!flagStr.empty()) {
                    if (flagStr.find("Persistent") != std::string::npos) {
                        flags = flags | runtime::PropertyFlag::Persistent;
                    }
                    if (flagStr.find("ClientSync") != std::string::npos) {
                        flags = flags | runtime::PropertyFlag::ClientSync;
                    }
                    if (flagStr.find("Base") != std::string::npos) {
                        flags = flags | runtime::PropertyFlag::Base;
                    }
                    if (flagStr.find("Cell") != std::string::npos) {
                        flags = flags | runtime::PropertyFlag::Cell;
                    }
                }

                std::vector<std::byte> defaultValue;
                auto defaultStr = findAttr(propNode, "defaultValue");
                if (!defaultStr.empty()) {
                    if (type == runtime::PropertyType::String) {
                        defaultValue.assign(
                            reinterpret_cast<const std::byte*>(defaultStr.data()),
                            reinterpret_cast<const std::byte*>(defaultStr.data()) + defaultStr.size());
                    } else if (type == runtime::PropertyType::Blob) {
                        // Hex-encoded blob: "DEADBEEF" -> {0xDE, 0xAD, 0xBE, 0xEF}
                        if (defaultStr.size() % 2 == 0) {
                            defaultValue.reserve(defaultStr.size() / 2);
                            for (std::size_t i = 0; i + 1 < defaultStr.size(); i += 2) {
                                auto byteVal = static_cast<std::uint8_t>(
                                    std::stoul(defaultStr.substr(i, 2), nullptr, 16));
                                defaultValue.push_back(static_cast<std::byte>(byteVal));
                            }
                        }
                    } else if (type == runtime::PropertyType::Vector3) {
                        // Format: "x,y,z"
                        defaultValue.resize(sizeof(float) * 3, std::byte{0});
                        float vals[3] = {0, 0, 0};
                        int idx = 0;
                        std::string token;
                        for (auto ch : defaultStr) {
                            if (ch == ',') {
                                if (idx < 3 && !token.empty()) {
                                    vals[idx] = std::stof(token);
                                    ++idx;
                                }
                                token.clear();
                            } else {
                                token += ch;
                            }
                        }
                        if (idx < 3 && !token.empty()) {
                            vals[idx] = std::stof(token);
                        }
                        std::memcpy(defaultValue.data(), vals, sizeof(float) * 3);
                    } else if (!runtime::EntityDef::isVariableSized(type)) {
                        auto fixedSize = runtime::EntityDef::fixedSizeOfType(type);
                        if (fixedSize > 0) {
                            defaultValue.resize(fixedSize, std::byte{0});
                            switch (type) {
                                case runtime::PropertyType::Int8:
                                case runtime::PropertyType::UInt8: {
                                    auto v = static_cast<std::uint8_t>(std::stoi(defaultStr));
                                    std::memcpy(defaultValue.data(), &v, 1);
                                    break;
                                }
                                case runtime::PropertyType::Int16:
                                case runtime::PropertyType::UInt16: {
                                    auto v = static_cast<std::uint16_t>(std::stoi(defaultStr));
                                    std::memcpy(defaultValue.data(), &v, 2);
                                    break;
                                }
                                case runtime::PropertyType::Int32:
                                case runtime::PropertyType::UInt32: {
                                    auto v = static_cast<std::uint32_t>(std::stoi(defaultStr));
                                    std::memcpy(defaultValue.data(), &v, 4);
                                    break;
                                }
                                case runtime::PropertyType::Int64:
                                case runtime::PropertyType::UInt64: {
                                    auto v = static_cast<std::uint64_t>(std::stoll(defaultStr));
                                    std::memcpy(defaultValue.data(), &v, 8);
                                    break;
                                }
                                case runtime::PropertyType::Float32: {
                                    auto v = std::stof(defaultStr);
                                    std::memcpy(defaultValue.data(), &v, 4);
                                    break;
                                }
                                case runtime::PropertyType::Float64: {
                                    auto v = std::stod(defaultStr);
                                    std::memcpy(defaultValue.data(), &v, 8);
                                    break;
                                }
                                case runtime::PropertyType::Bool: {
                                    auto v = static_cast<std::uint8_t>(defaultStr == "true" ? 1 : 0);
                                    std::memcpy(defaultValue.data(), &v, 1);
                                    break;
                                }
                                default:
                                    break;
                            }
                        }
                    }
                }

                def->addProperty(std::move(name), type, 0, flags, std::move(defaultValue));
            }
        } else if (child.tag == "Methods") {
            for (auto& methNode : child.children) {
                if (methNode.tag != "Method") continue;

                auto name = findAttr(methNode, "name");
                if (name.empty()) {
                    throw std::runtime_error("Method missing 'name' attribute");
                }

                auto sideStr = findAttr(methNode, "side", "Cell");
                auto side = runtime::MethodSide::Cell;
                if (sideStr == "Base") side = runtime::MethodSide::Base;
                else if (sideStr == "Client") side = runtime::MethodSide::Client;

                std::vector<runtime::ArgDescriptor> args;
                for (auto& argNode : methNode.children) {
                    if (argNode.tag != "Arg") continue;
                    auto argName = findAttr(argNode, "name");
                    auto argTypeStr = findAttr(argNode, "type", "Int32");
                    auto argType = parsePropertyType(argTypeStr);
                    args.push_back({std::move(argName), argType});
                }

                def->addMethod(std::move(name), side, std::move(args));
            }
        }
    }

    return def;
}

std::unique_ptr<EntityDef> EntityDefLoader::loadFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + path);
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    return loadFromString(ss.str());
}

}  // namespace theseed::core
