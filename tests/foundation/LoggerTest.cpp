#include "theseed/foundation/Logger.h"

#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using theseed::foundation::ConsoleLogger;
using theseed::foundation::ILogger;
using theseed::foundation::LogAttribute;
using theseed::foundation::LogLevel;
using theseed::foundation::LogRecord;
using theseed::foundation::globalLogger;
using theseed::foundation::levelName;
using theseed::foundation::logDebug;
using theseed::foundation::logError;
using theseed::foundation::logInfo;
using theseed::foundation::logWarn;
using theseed::foundation::setGlobalLogger;
using theseed::foundation::takeGlobalLogger;

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name)                                              \
    do {                                                        \
        std::cout << "  " << (name) << "... " << std::flush;    \
    } while (0)

#define PASS()                                                  \
    do {                                                        \
        std::cout << "OK\n";                                    \
        ++testsPassed;                                          \
    } while (0)

#define FAIL(msg)                                               \
    do {                                                        \
        std::cout << "FAILED: " << (msg) << "\n";               \
        ++testsFailed;                                          \
    } while (0)

static std::string capture(LogLevel level, LogRecord record) {
    std::ostringstream os;
    ConsoleLogger logger(level, &os);
    logger.log(std::move(record));
    return os.str();
}

static void testLogLevelFilter() {
    TEST("console logger level filtering");

    LogRecord debugRec;
    debugRec.level = LogLevel::Debug;
    debugRec.message = "dbg";
    auto debugOut = capture(LogLevel::Warn, debugRec);

    LogRecord infoRec;
    infoRec.level = LogLevel::Info;
    infoRec.message = "inf";
    auto infoOut = capture(LogLevel::Warn, infoRec);

    LogRecord warnRec;
    warnRec.level = LogLevel::Warn;
    warnRec.message = "wrn";
    auto warnOut = capture(LogLevel::Warn, warnRec);

    LogRecord errorRec;
    errorRec.level = LogLevel::Error;
    errorRec.message = "err";
    auto errorOut = capture(LogLevel::Warn, errorRec);

    bool ok = debugOut.empty() && infoOut.empty();
    ok = ok && !warnOut.empty() && !errorOut.empty();
    ok = ok && warnOut.find("wrn") != std::string::npos;
    ok = ok && errorOut.find("err") != std::string::npos;

    if (ok) PASS(); else FAIL("level filter incorrect");
}

static void testConsoleLoggerFormat() {
    TEST("console logger format includes timestamp, level, message");

    LogRecord rec;
    rec.level = LogLevel::Info;
    rec.message = "hello world";
    auto out = capture(LogLevel::Debug, rec);

    bool ok = out.find("INFO") != std::string::npos;
    ok = ok && out.find("hello world") != std::string::npos;
    // ISO8601 marker "T" between date and time
    ok = ok && out.find('T') != std::string::npos;
    ok = ok && out.find(":") != std::string::npos;

    if (ok) PASS(); else FAIL("format missing fields: " + out);
}

static void testStructuredAttributes() {
    TEST("structured attributes serialize as key=value");

    LogRecord rec;
    rec.level = LogLevel::Info;
    rec.message = "attrs-test";
    rec.attrs = {
        {"entity", std::string("Avatar")},
        {"count", std::int64_t{42}},
        {"ratio", 3.5},
        {"alive", true},
    };
    auto out = capture(LogLevel::Debug, rec);

    bool ok = out.find("entity=\"Avatar\"") != std::string::npos;
    ok = ok && out.find("count=42") != std::string::npos;
    ok = ok && out.find("ratio=3.5") != std::string::npos;
    ok = ok && out.find("alive=true") != std::string::npos;

    if (ok) PASS(); else FAIL("attribute serialization wrong: " + out);
}

static void testGlobalLoggerReplace() {
    TEST("setGlobalLogger routes calls to custom logger");

    class CapturingLogger final : public ILogger {
    public:
        void log(LogRecord record) override {
            captured += record.message;
            captured += '|';
        }
        void setLevel(LogLevel) override {}
        LogLevel level() const override { return LogLevel::Debug; }
        std::string captured;
    };

    auto original = takeGlobalLogger();
    auto custom = std::make_shared<CapturingLogger>();
    setGlobalLogger(custom);

    logInfo("ping");

    setGlobalLogger(original);

    if (custom->captured.find("ping") != std::string::npos) PASS();
    else FAIL("custom logger was not invoked");
}

static void testConvenienceFunctions() {
    TEST("convenience free functions log at correct level");

    std::ostringstream os;
    auto logger = std::make_shared<ConsoleLogger>(LogLevel::Debug, &os);

    auto original = takeGlobalLogger();
    setGlobalLogger(logger);

    os.str("");
    os.clear();
    logDebug("d-msg");
    bool ok = os.str().find("DEBUG") != std::string::npos && os.str().find("d-msg") != std::string::npos;

    os.str("");
    os.clear();
    logInfo("i-msg");
    ok = ok && os.str().find("INFO") != std::string::npos && os.str().find("i-msg") != std::string::npos;

    os.str("");
    os.clear();
    logWarn("w-msg");
    ok = ok && os.str().find("WARN") != std::string::npos && os.str().find("w-msg") != std::string::npos;

    os.str("");
    os.clear();
    logError("e-msg");
    ok = ok && os.str().find("ERROR") != std::string::npos && os.str().find("e-msg") != std::string::npos;

    setGlobalLogger(original);

    if (ok) PASS(); else FAIL("convenience functions did not log correctly");
}

static void testTraceSpanOptional() {
    TEST("trace_id / span_id emitted only when present");

    LogRecord withTrace;
    withTrace.level = LogLevel::Info;
    withTrace.message = "tr";
    withTrace.traceId = "abc123";
    withTrace.spanId = "def456";
    auto withOut = capture(LogLevel::Debug, withTrace);

    bool ok = withOut.find("trace=abc123") != std::string::npos;
    ok = ok && withOut.find("span=def456") != std::string::npos;

    LogRecord noTrace;
    noTrace.level = LogLevel::Info;
    noTrace.message = "tr";
    auto noOut = capture(LogLevel::Debug, noTrace);
    ok = ok && noOut.find("trace=") == std::string::npos;
    ok = ok && noOut.find("span=") == std::string::npos;

    if (ok) PASS(); else FAIL("trace/span handling wrong");
}

static void testConsoleLoggerLevelAccessors() {
    TEST("console logger setLevel/level round trip");

    ConsoleLogger logger(LogLevel::Warn);
    bool ok = logger.level() == LogLevel::Warn;
    logger.setLevel(LogLevel::Error);
    ok = ok && logger.level() == LogLevel::Error;
    logger.setLevel(LogLevel::Debug);
    ok = ok && logger.level() == LogLevel::Debug;

    if (ok) PASS(); else FAIL("level accessors mismatch");
}

// 字符串属性中的引号与反斜杠必须转义输出（formatValue 的两条转义臂）。
static void testStringAttributeEscaping() {
    TEST("string attribute escapes quotes and backslashes");

    LogRecord rec;
    rec.level = LogLevel::Info;
    rec.message = "esc";
    const std::string tricky = std::string("a\"b\\c");
    rec.attrs = {{"payload", tricky}};
    auto out = capture(LogLevel::Debug, rec);

    bool ok = out.find("payload=\"a\\\"b\\\\c\"") != std::string::npos;
    if (ok) PASS(); else FAIL("escaping wrong: " + out);
}

// bool=false 属性渲染 "false"（ternary 的另一臂）。
static void testBoolFalseAttribute() {
    TEST("bool false attribute renders false");

    LogRecord rec;
    rec.level = LogLevel::Info;
    rec.message = "bool";
    rec.attrs = {{"alive", false}};
    auto out = capture(LogLevel::Debug, rec);

    bool ok = out.find("alive=false") != std::string::npos;
    if (ok) PASS(); else FAIL("expected alive=false: " + out);
}

// 枚举外值走 default 臂的 UNKNOWN 兜底。
static void testLevelNameUnknownFallback() {
    TEST("levelName returns UNKNOWN for out-of-range level");

    const auto weird = static_cast<LogLevel>(99);
    bool ok = std::string(levelName(weird)) == "UNKNOWN";
    ok = ok && std::string(levelName(LogLevel::Error)) == "ERROR";
    if (ok) PASS(); else FAIL("unknown level fallback wrong");
}

// 空 logger 入参：setGlobalLogger 保持原状（if (logger) 假臂）。
static void testSetGlobalLoggerNullptrIsNoop() {
    TEST("setGlobalLogger(nullptr) keeps current logger");

    auto original = takeGlobalLogger();
    setGlobalLogger(original);            // 先把原件设回"当前"
    setGlobalLogger(nullptr);             // if (logger) 假臂：应为 noop
    auto after = takeGlobalLogger();

    bool ok = after.get() == original.get();  // 指针未被替换
    setGlobalLogger(original);
    if (ok) PASS(); else FAIL("null logger should not replace storage");
}

int main() {
    std::cout << "Logger tests:\n";

    testLogLevelFilter();
    testConsoleLoggerFormat();
    testStructuredAttributes();
    testStringAttributeEscaping();
    testBoolFalseAttribute();
    testLevelNameUnknownFallback();
    testSetGlobalLoggerNullptrIsNoop();
    testGlobalLoggerReplace();
    testConvenienceFunctions();
    testTraceSpanOptional();
    testConsoleLoggerLevelAccessors();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
