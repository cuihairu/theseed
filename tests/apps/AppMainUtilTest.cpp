#include "AppMainUtil.h"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>

static int failed = 0;
#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { std::cout << "  FAILED: " << msg << "\n"; ++failed; } \
    } while (0)

int main() {
    const auto okInt = theseed::app::parseInt("42");
    CHECK(okInt.has_value() && *okInt == 42, "parseInt 42");

    CHECK(!theseed::app::parseInt("").has_value(), "parseInt empty rejected");
    CHECK(!theseed::app::parseInt("abc").has_value(), "parseInt non-numeric rejected");
    CHECK(!theseed::app::parseInt("99999999999999999999").has_value(),
          "parseInt overflow rejected");
    // 深审回归：std::stoi 接受最长有效前缀，畸形串被静默截断。修复后
    // 全部字符串须被消费，部分解析一律拒绝。
    CHECK(!theseed::app::parseInt("42abc").has_value(), "parseInt trailing junk rejected");
    CHECK(!theseed::app::parseInt(" 42").has_value(), "parseInt leading space rejected");
    CHECK(!theseed::app::parseInt("+42").has_value(), "parseInt plus sign rejected");
    CHECK(!theseed::app::parseInt("0x10").has_value(), "parseInt hex prefix rejected");

    const auto okPort = theseed::app::parsePort("65535");
    CHECK(okPort.has_value() && *okPort == 65535, "parsePort 65535");
    CHECK(!theseed::app::parsePort("65536").has_value(), "parsePort >65535 rejected");
    CHECK(!theseed::app::parsePort("-1").has_value(), "parsePort negative rejected");
    CHECK(!theseed::app::parsePort("abc").has_value(), "parsePort non-numeric rejected");
    // 端口 0 边界：parsePort 允许 0（语义：显式绑定/自动分配保留），
    // 与 >65535、负值边界共同覆盖。
    const auto okZero = theseed::app::parsePort("0");
    CHECK(okZero.has_value() && *okZero == 0, "parsePort 0 allowed");

    CHECK(theseed::app::gRunning == 1, "gRunning starts true");
    theseed::app::gRunning = 0;
    CHECK(theseed::app::gRunning == 0, "gRunning writable");

    std::cout << (failed == 0 ? "AppMainUtilTest: all passed" : "AppMainUtilTest: FAILED")
              << std::endl;
    return failed == 0 ? 0 : 1;
}
