#include "theseed/core/EntityDefLoader.h"
#include "theseed/runtime/EntityDef.h"

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
                def->addProperty(std::move(name), type);
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

                def->addMethod(std::move(name), side);
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
