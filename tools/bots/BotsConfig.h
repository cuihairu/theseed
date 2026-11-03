#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace theseed::bots {

// 斜坡压测配置（KBEngine <bots> 配置思想的 key=value 版）。
//   斜坡三件套：totalCount / tickTime / tickCount —— 每 tickTime 毫秒
//   新增 tickCount 个 bot，直到 totalCount。
struct BotsConfig final {
    // 接入目标
    std::string loginHost = "127.0.0.1";
    std::uint16_t loginPort = 20099;
    // BaseApp 覆盖地址。留空/0 时用 SelectRealmResponse 返回的 host/port。
    std::string baseHost;
    std::uint16_t basePort = 0;
    std::string realmId = "default";

    // 账号
    std::string accountPrefix = "bot_";
    std::string accountPassword;

    // 斜坡三件套
    std::size_t totalCount = 10;
    std::chrono::milliseconds tickTime{1000};
    std::size_t tickCount = 1;

    // 每 tick 行为脚本（数据驱动，无 Lua 依赖）
    std::chrono::milliseconds moveInterval{200};
    std::chrono::milliseconds heartbeatInterval{1000};
    std::string moveAction = "onDamage";      // EntityDef 里 Cell 侧方法
    std::string heartbeatAction = "onHeal";   // EntityDef 里 Base 侧方法

    // 断线重连退避 + 统计打印周期 + 运行时长（0 = 直到 Ctrl+C）
    std::chrono::milliseconds reconnectDelay{2000};
    std::chrono::milliseconds statsInterval{1000};
    std::chrono::seconds runTime{0};
    // 管理器主循环粒度（也是 connect/pump/动作的调度分辨率）
    std::chrono::milliseconds tickInterval{10};
};

// 解析 key = value 文本配置。'#' / ';' 行注释；未知键报错（防拼写静默失效）；
// 端口/数量等字段做范围校验。失败时返回 false 并填充 outError。
bool parseBotsConfig(const std::string& text, BotsConfig& outConfig,
                     std::string& outError);

// 便捷重载：从文件读取。
bool loadBotsConfig(const std::string& path, BotsConfig& outConfig,
                    std::string& outError);

}  // namespace theseed::bots
