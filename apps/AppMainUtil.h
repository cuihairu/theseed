#pragma once

// 各 app main 共用的命令行/信号基础设施。
//
// 两个深审问题的公共根因：
//  1. `volatile bool g_running` 不是信号处理器可安全写入的类型。C/C++ 标准
//     只保证信号上下文中对 `volatile sig_atomic_t` 的读写是原子的；`volatile
//     bool` 无此保证，编译器可将其暂存寄存器（信号打断的写可能丢失）或生成
//     撕裂访问。改用 `volatile std::sig_atomic_t`。
//  2. `std::stoi` 解析失败抛 `std::invalid_argument`/`std::out_of_range`，
//     app main 若不加 catch，异常会逃逸 main 触发 std::terminate（无栈清理）。
//     统一经 `parseInt`/`parsePort` 转成 optional，失败打印诊断并让调用方
//     返回 1。

#include <csignal>
#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <string>

namespace theseed::app {

inline volatile std::sig_atomic_t gRunning = 1;

inline void onSignal(int) { gRunning = 0; }

inline void installSignalHandlers() {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
}

inline std::optional<int> parseInt(const std::string& value) {
    int parsed = 0;
    const auto [ptr, ec] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    // std::from_chars 不跳过前导空白、不接受 '+' / 十六进制前缀，且仅当
    // 整个字符串被消费（ptr == end）才算合法；"42abc"→"42" 这类最长有效
    // 前缀截断解析因此被拒绝，端口/组件号不再被静默截断。
    if (ec != std::errc() || ptr != value.data() + value.size()) {
        std::cerr << "Invalid numeric argument: " << value << "\n";
        return std::nullopt;
    }
    return parsed;
}

inline std::optional<std::uint16_t> parsePort(const std::string& value) {
    const auto parsed = parseInt(value);
    if (!parsed) return std::nullopt;
    if (*parsed < 0 || *parsed > 65535) {
        std::cerr << "Port out of range: " << value << "\n";
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(*parsed);
}

}  // namespace theseed::app
