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

    const auto okPort = theseed::app::parsePort("65535");
    CHECK(okPort.has_value() && *okPort == 65535, "parsePort 65535");
    CHECK(!theseed::app::parsePort("65536").has_value(), "parsePort >65535 rejected");
    CHECK(!theseed::app::parsePort("-1").has_value(), "parsePort negative rejected");
    CHECK(!theseed::app::parsePort("abc").has_value(), "parsePort non-numeric rejected");

    CHECK(theseed::app::gRunning == 1, "gRunning starts true");
    theseed::app::gRunning = 0;
    CHECK(theseed::app::gRunning == 0, "gRunning writable");

    std::cout << (failed == 0 ? "AppMainUtilTest: all passed" : "AppMainUtilTest: FAILED")
              << std::endl;
    return failed == 0 ? 0 : 1;
}
