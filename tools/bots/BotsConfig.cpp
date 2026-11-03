#include "BotsConfig.h"

#include <charconv>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>

namespace theseed::bots {

namespace {

bool parseCount(const std::string& value, std::size_t min, std::size_t max,
                std::size_t& out) {
    std::size_t v = 0;
    auto res = std::from_chars(value.data(), value.data() + value.size(), v);
    if (res.ec != std::errc() || res.ptr != value.data() + value.size()) return false;
    if (v < min || v > max) return false;
    out = v;
    return true;
}

bool parseMs(const std::string& value, std::int64_t min, std::int64_t max,
             std::chrono::milliseconds& out) {
    std::int64_t v = 0;
    auto res = std::from_chars(value.data(), value.data() + value.size(), v);
    if (res.ec != std::errc() || res.ptr != value.data() + value.size()) return false;
    if (v < min || v > max) return false;
    out = std::chrono::milliseconds{v};
    return true;
}

}  // namespace

bool parseBotsConfig(const std::string& text, BotsConfig& outConfig,
                     std::string& outError) {
    BotsConfig cfg;  // 从默认值开始，逐键覆盖
    std::map<std::string, std::pair<std::string, std::size_t>> seen;  // key -> (value, lineNo)

    std::istringstream in(text);
    std::string line;
    std::size_t lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        const auto hash = line.find_first_of("#;");
        if (hash != std::string::npos) line = line.substr(0, hash);
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            outError = "line " + std::to_string(lineNo) + ": expected 'key = value'";
            return false;
        }
        // 键：[first, eq) 去尾随空白（注意 find_last_not_of 从 pos 起反向
        // 搜索，pos=eq 处是 '=' 本身，必须从 eq-1 起手）。
        std::size_t keyEnd = eq;
        while (keyEnd > first && (line[keyEnd - 1] == ' ' || line[keyEnd - 1] == '\t')) {
            --keyEnd;
        }
        if (keyEnd == first) {
            outError = "line " + std::to_string(lineNo) + ": empty key";
            return false;
        }
        const auto key = line.substr(first, keyEnd - first);
        auto vfirst = line.find_first_not_of(" \t", eq + 1);
        std::string value;
        if (vfirst != std::string::npos) {
            value = line.substr(vfirst, line.find_last_not_of(" \t\r") - vfirst + 1);
        }
        if (!seen.emplace(key, std::make_pair(value, lineNo)).second) {
            outError = "line " + std::to_string(lineNo) + ": duplicate key '" + key + "'";
            return false;
        }
    }

    auto asStr = [&](const char* k, std::string& out) {
        out = seen[k].first;
        return true;
    };
    auto asCount = [&](const char* k, std::size_t min, std::size_t max, std::size_t& out) {
        return parseCount(seen[k].first, min, max, out)
            || (outError = "key '" + std::string(k) + "': expected integer "
                           + std::to_string(min) + ".." + std::to_string(max)
                           + " (line " + std::to_string(seen[k].second) + ")", false);
    };
    auto asMs = [&](const char* k, std::int64_t min, std::int64_t max,
                    std::chrono::milliseconds& out) {
        return parseMs(seen[k].first, min, max, out)
            || (outError = "key '" + std::string(k) + "': expected milliseconds "
                           + std::to_string(min) + ".." + std::to_string(max)
                           + " (line " + std::to_string(seen[k].second) + ")", false);
    };

    static const std::map<std::string, int> kKnown = {
        {"login_host", 0}, {"login_port", 0}, {"base_host", 0}, {"base_port", 0},
        {"realm_id", 0}, {"account_prefix", 0}, {"account_password", 0},
        {"total_count", 0}, {"tick_time", 0}, {"tick_count", 0},
        {"move_interval", 0}, {"heartbeat_interval", 0},
        {"move_action", 0}, {"heartbeat_action", 0},
        {"reconnect_delay", 0}, {"stats_interval", 0},
        {"run_time", 0}, {"tick_interval", 0},
    };
    for (const auto& [key, meta] : seen) {
        if (!kKnown.count(key)) {
            outError = "line " + std::to_string(meta.second) + ": unknown key '" + key + "'";
            return false;
        }
    }

    if (seen.count("login_host")) asStr("login_host", cfg.loginHost);
    if (seen.count("base_host")) asStr("base_host", cfg.baseHost);
    if (seen.count("realm_id")) asStr("realm_id", cfg.realmId);
    if (seen.count("account_prefix")) asStr("account_prefix", cfg.accountPrefix);
    if (seen.count("account_password")) asStr("account_password", cfg.accountPassword);
    if (seen.count("move_action")) asStr("move_action", cfg.moveAction);
    if (seen.count("heartbeat_action")) asStr("heartbeat_action", cfg.heartbeatAction);

    std::size_t portNum = 0;
    if (seen.count("login_port")) {
        if (!asCount("login_port", 1, 65535, portNum)) return false;
        cfg.loginPort = static_cast<std::uint16_t>(portNum);
    }
    if (seen.count("base_port")) {
        if (!asCount("base_port", 0, 65535, portNum)) return false;
        cfg.basePort = static_cast<std::uint16_t>(portNum);
    }
    if (seen.count("total_count") && !asCount("total_count", 1, 100000, cfg.totalCount)) return false;
    if (seen.count("tick_count") && !asCount("tick_count", 1, 100000, cfg.tickCount)) return false;
    if (seen.count("tick_time") && !asMs("tick_time", 1, 3600000, cfg.tickTime)) return false;
    if (seen.count("move_interval") && !asMs("move_interval", 1, 3600000, cfg.moveInterval)) return false;
    if (seen.count("heartbeat_interval") && !asMs("heartbeat_interval", 1, 3600000, cfg.heartbeatInterval)) return false;
    if (seen.count("reconnect_delay") && !asMs("reconnect_delay", 1, 3600000, cfg.reconnectDelay)) return false;
    if (seen.count("stats_interval") && !asMs("stats_interval", 1, 3600000, cfg.statsInterval)) return false;
    if (seen.count("tick_interval") && !asMs("tick_interval", 1, 1000, cfg.tickInterval)) return false;
    if (seen.count("run_time")) {
        std::int64_t sec = 0;
        auto v = seen["run_time"].first;
        auto res = std::from_chars(v.data(), v.data() + v.size(), sec);
        if (res.ec != std::errc() || res.ptr != v.data() + v.size() || sec < 0 || sec > 86400 * 30) {
            outError = "key 'run_time': expected seconds 0..2592000 (line "
                       + std::to_string(seen["run_time"].second) + ")";
            return false;
        }
        cfg.runTime = std::chrono::seconds{sec};
    }

    outConfig = std::move(cfg);
    outError.clear();
    return true;
}

bool loadBotsConfig(const std::string& path, BotsConfig& outConfig,
                    std::string& outError) {
    std::ifstream file(path);
    if (!file) {
        outError = "cannot open config file: " + path;
        return false;
    }
    std::ostringstream buf;
    buf << file.rdbuf();
    return parseBotsConfig(buf.str(), outConfig, outError);
}

}  // namespace theseed::bots
