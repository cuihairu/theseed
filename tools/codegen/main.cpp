// theseed-codegen: 离线 EntityDef → C# (Unity) 客户端代码生成器。
//
// 用法:
//   theseed-codegen --defs ./res/entities/ --output-unity ./client/unity/Assets/Theseed/Generated/
//
// MVP 范围（与 docs/design/0-foundation/01-mvp-architecture-baseline.md §11 对齐）：
//   - Entity partial class 生成
//   - Exposed 方法桩（CallServer）+ Client 回调 partial 桩
//   - 基础序列化（BinaryWriter / BinaryReader）

#include "theseed/codegen/CSharpEmitter.h"
#include "theseed/core/EntityDefLoader.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using theseed::codegen::CSharpEmitter;
using theseed::core::EntityDefLoader;

namespace {

struct CliArgs {
    fs::path defsDir;
    fs::path outputUnityDir;
    std::string namespaceName = "Theseed.Generated";
};

void printUsage() {
    std::cerr <<
        "Usage: theseed-codegen --defs <dir> --output-unity <dir> "
        "[--namespace <name>]\n";
}

bool parseArgs(int argc, char** argv, CliArgs& out) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) return {};
            return argv[++i];
        };
        if (arg == "--defs") {
            out.defsDir = next();
        } else if (arg == "--output-unity") {
            out.outputUnityDir = next();
        } else if (arg == "--namespace") {
            out.namespaceName = next();
        } else if (arg == "--help" || arg == "-h") {
            printUsage();
            return false;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            printUsage();
            return false;
        }
    }
    if (out.defsDir.empty() || out.outputUnityDir.empty()) {
        printUsage();
        return false;
    }
    return true;
}

std::vector<std::unique_ptr<theseed::runtime::EntityDef>> loadAllDefs(const fs::path& dir) {
    std::vector<std::unique_ptr<theseed::runtime::EntityDef>> defs;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".xml") continue;
        defs.push_back(EntityDefLoader::loadFromFile(entry.path().string()));
    }
    return defs;
}

}  // namespace

int main(int argc, char** argv) {
    CliArgs args;
    if (!parseArgs(argc, argv, args)) return 1;

    std::error_code ec;
    if (!fs::exists(args.defsDir, ec)) {
        std::cerr << "Defs directory does not exist: " << args.defsDir << "\n";
        return 1;
    }

    auto owned = loadAllDefs(args.defsDir);
    std::vector<const theseed::runtime::EntityDef*> defs;
    defs.reserve(owned.size());
    for (const auto& d : owned) defs.push_back(d.get());

    theseed::codegen::CSharpEmitOptions opts;
    opts.namespaceName = args.namespaceName;
    opts.emitHeader = true;
    CSharpEmitter emitter(opts);

    auto files = emitter.emit(defs);

    fs::create_directories(args.outputUnityDir, ec);
    if (ec) {
        std::cerr << "Failed to create output directory: " << args.outputUnityDir
                  << " (" << ec.message() << ")\n";
        return 1;
    }

    for (const auto& [filename, content] : files) {
        fs::path path = args.outputUnityDir / filename;
        std::ofstream out(path, std::ios::binary);
        if (!out.is_open()) {
            std::cerr << "Failed to open output file: " << path << "\n";
            return 1;
        }
        out << content;
        std::cout << "Wrote " << path << " (" << content.size() << " bytes)\n";
    }

    return 0;
}
