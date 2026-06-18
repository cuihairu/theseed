#include "theseed/foundation/Logger.h"
#include "theseed/foundation/Tracing.h"

#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <utility>

namespace theseed::foundation {

namespace {

std::shared_ptr<ILogger>& globalStorage() {
    static std::shared_ptr<ILogger> logger = std::make_shared<ConsoleLogger>();
    return logger;
}

std::mutex& globalMutex() {
    static std::mutex m;
    return m;
}

void formatValue(std::ostream& out, const LogAttribute::Value& v) {
    std::visit([&](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::string>) {
            out << '"';
            for (char c : x) {
                if (c == '"' || c == '\\') out << '\\';
                out << c;
            }
            out << '"';
        } else if constexpr (std::is_same_v<T, bool>) {
            out << (x ? "true" : "false");
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            out << x;
        } else {
            out << x;
        }
    }, v);
}

std::string formatTimestamp(const std::chrono::system_clock::time_point& tp) {
    const auto secs = std::chrono::time_point_cast<std::chrono::seconds>(tp);
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(tp - secs).count();
    const std::time_t t = std::chrono::system_clock::to_time_t(secs);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(6) << (ns / 1000);
    return out.str();
}

}  // namespace

const char* levelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "UNKNOWN";
}

ConsoleLogger::ConsoleLogger(LogLevel level, std::ostream* sink)
    : level_(level), sink_(sink ? sink : &std::cerr) {}

void ConsoleLogger::log(LogRecord record) {
    if (static_cast<std::uint8_t>(record.level) < static_cast<std::uint8_t>(level_)) {
        return;
    }
    static std::mutex ioMutex;
    const std::lock_guard lock(ioMutex);
    auto& out = *sink_;
    out << formatTimestamp(record.timestamp)
        << " [" << levelName(record.level) << ']';

    if (record.traceId) {
        out << " trace=" << *record.traceId;
    }
    if (record.spanId) {
        out << " span=" << *record.spanId;
    }

    out << ' ' << record.message;

    for (const auto& attr : record.attrs) {
        out << ' ' << attr.key << '=';
        formatValue(out, attr.value);
    }
    out << '\n';
}

void ConsoleLogger::setLevel(LogLevel level) { level_ = level; }
LogLevel ConsoleLogger::level() const { return level_; }

ILogger& globalLogger() {
    const std::lock_guard lock(globalMutex());
    return *globalStorage();
}

void setGlobalLogger(std::shared_ptr<ILogger> logger) {
    const std::lock_guard lock(globalMutex());
    if (logger) {
        globalStorage() = std::move(logger);
    }
}

std::shared_ptr<ILogger> takeGlobalLogger() {
    const std::lock_guard lock(globalMutex());
    auto taken = globalStorage();
    globalStorage() = std::make_shared<ConsoleLogger>();
    return taken;
}

namespace {

LogRecord makeRecord(LogLevel level, std::string msg, std::vector<LogAttribute> attrs) {
    LogRecord r;
    r.level = level;
    r.message = std::move(msg);
    r.attrs = std::move(attrs);
    const auto ctx = currentSpanContext();
    if (ctx.isValid()) {
        r.traceId = ctx.traceId;
        r.spanId = ctx.spanId;
    }
    return r;
}

}  // namespace

void logDebug(std::string msg, std::vector<LogAttribute> attrs) {
    globalLogger().log(makeRecord(LogLevel::Debug, std::move(msg), std::move(attrs)));
}

void logInfo(std::string msg, std::vector<LogAttribute> attrs) {
    globalLogger().log(makeRecord(LogLevel::Info, std::move(msg), std::move(attrs)));
}

void logWarn(std::string msg, std::vector<LogAttribute> attrs) {
    globalLogger().log(makeRecord(LogLevel::Warn, std::move(msg), std::move(attrs)));
}

void logError(std::string msg, std::vector<LogAttribute> attrs) {
    globalLogger().log(makeRecord(LogLevel::Error, std::move(msg), std::move(attrs)));
}

}  // namespace theseed::foundation
