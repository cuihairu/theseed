// FileEntityStore 分支覆盖测试（第四批）：allocId 元文件打不开的防御臂、
// listIdsByType / listEntityTypes 对混合目录内容（子目录、非 .dat 文件、
// 非数字文件名、下划线前缀目录）的跳过臂。与 FileEntityStoreTest 互补——
// 那边收功能回环，这边专收目录遍历与文件打开的防御分支。
#include "theseed/core/EntityData.h"
#include "theseed/core/FileEntityStore.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <system_error>
#include <vector>

using theseed::core::DataType;
using theseed::core::EntityData;
using theseed::core::FileEntityStore;

namespace {

int gFailures = 0;

#define CHECK(cond, msg)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            std::cout << "  FAILED: " << msg << std::endl;   \
            ++gFailures;                                     \
        }                                                    \
    } while (0)

namespace fs = std::filesystem;

EntityData makeAvatar(theseed::core::EntityId id) {
    EntityData data;
    data.id = id;
    data.entityType = "Avatar";
    auto& prop = data.properties.emplace_back();
    prop.id = 0;
    prop.name = "level";
    prop.type = DataType::Int32;
    const std::int32_t level = 7;
    prop.rawValue.assign(reinterpret_cast<const std::byte*>(&level),
                         reinterpret_cast<const std::byte*>(&level) + sizeof(level));
    return data;
}

void writeBytes(const fs::path& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << content;
}

}  // namespace

int main() {
    const fs::path root = "test_file_store_branch_root";
    std::error_code ec;
    fs::remove_all(root, ec);

    // allocId 元文件防御臂：
    //  (a) _next_id.dat 存在但打不开（权限清零）→ 读失败臂（108 的 false 臂），
    //      nextId 回退 1。root 身份跑时权限不生效，会读出真实值 6，两者都算过臂。
    //  (b) _next_id.dat 是目录 → 读失败 nextId=1，写打开失败臂（120 的 false 臂）。
    {
        FileEntityStore store(root);
        CHECK(store.save(1, makeAvatar(1)), "seed save");

        const auto meta = root / "_next_id.dat";
        // 写入 nextId=6（已分配 1 后的预期值），供权限清零场景对照。
        {
            std::uint64_t nextId = 6;
            std::ofstream f(meta, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(&nextId), sizeof(nextId));
        }
        fs::permissions(meta, fs::perms::none, ec);
        const auto idA = store.allocId();
        CHECK(idA == 1 || idA == 6, "allocId after unreadable meta (1 as user, 6 as root)");
        fs::permissions(meta, fs::perms::owner_read | fs::perms::owner_write, ec);

        fs::remove(meta, ec);
        fs::create_directory(meta, ec);
        CHECK(store.allocId() == 1, "allocId with directory meta falls back to 1");
        fs::remove_all(root, ec);
    }

    // listIdsByType 混合目录内容：子目录、非 .dat 文件、非数字文件名都被跳过。
    {
        FileEntityStore store(root);
        CHECK(store.save(1, makeAvatar(1)), "save 1");
        CHECK(store.save(2, makeAvatar(2)), "save 2");

        const auto dir = root / "Avatar";
        fs::create_directory(dir / "subdir", ec);            // 子目录 → is_regular_file 跳过
        writeBytes(dir / "junk.txt", "x");                   // 扩展名非 .dat → 跳过
        writeBytes(dir / "bad.dat", "xx");                   // 文件名非数字 → catch 跳过

        const auto ids = store.listIdsByType("Avatar");
        CHECK(ids.size() == 2 && ids[0] == 1 && ids[1] == 2,
              "listIdsByType skips non-entity entries");

        // 不存在的类型目录：早退臂。
        CHECK(store.listIdsByType("NoSuchType").empty(), "listIdsByType missing dir");
        fs::remove_all(root, ec);
    }

    // listEntityTypes 混合内容：下划线前缀目录与普通文件都被跳过。
    {
        FileEntityStore store(root);
        CHECK(store.save(1, makeAvatar(1)), "seed for types");

        fs::create_directory(root / "_meta", ec);            // '_' 前缀 → 跳过
        writeBytes(root / "loose.txt", "x");                 // 非目录 → 跳过

        const auto types = store.listEntityTypes();
        bool hasAvatar = false;
        bool hasMeta = false;
        for (const auto& t : types) {
            if (t == "Avatar") hasAvatar = true;
            if (t == "_meta") hasMeta = true;
        }
        CHECK(hasAvatar, "listEntityTypes keeps entity dirs");
        CHECK(!hasMeta, "listEntityTypes skips underscore dirs");
        fs::remove_all(root, ec);
    }

    if (gFailures == 0) {
        std::cout << "FileEntityStoreBranchTest: all passed" << std::endl;
    } else {
        std::cout << "FileEntityStoreBranchTest: " << gFailures << " failure(s)" << std::endl;
    }
    return gFailures == 0 ? 0 : 1;
}
