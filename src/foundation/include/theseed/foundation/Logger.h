#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <variant>
#include <vector>

namespace theseed::foundation {

enum class LogLevel : std::uint8_t {
    Debug = 0,
    Info,
    Warn,
    Error,
};

struct LogAttribute final {
    using Value = std::variant<std::string, std::int64_t, double, bool>;

    std::string key;
    Value value;
};

struct LogRecord final {
    LogLevel level = LogLevel::Info;
    std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();
    std::string message;
    std::vector<LogAttribute> attrs;
    std::optional<std::string> traceId;
    std::optional<std::string> spanId;
};

class ILogger {
public:
    virtual ~ILogger() = default;

    virtual void log(LogRecord record) = 0;
    virtual void setLevel(LogLevel level) = 0;
    virtual LogLevel level() const = 0;
};

// Default ILogger writing structured records to an injectable ostream (nullptr falls back to std::cerr).
class ConsoleLogger final : public ILogger {
public:
    explicit ConsoleLogger(LogLevel level = LogLevel::Info,
                           std::ostream* sink = nullptr);

    void log(LogRecord record) override;
    void setLevel(LogLevel level) override;
    LogLevel level() const override;

private:
    mutable LogLevel level_;
    std::ostream* sink_;
};

// Process-wide accessor. Replaces default ConsoleLogger if setGlobalLogger was called.
ILogger& globalLogger();
void setGlobalLogger(std::shared_ptr<ILogger> logger);
std::shared_ptr<ILogger> takeGlobalLogger();

// Convenience free functions for the most common call-site shape.
void logDebug(std::string msg, std::vector<LogAttribute> attrs = {});
void logInfo(std::string msg, std::vector<LogAttribute> attrs = {});
void logWarn(std::string msg, std::vector<LogAttribute> attrs = {});
void logError(std::string msg, std::vector<LogAttribute> attrs = {});

const char* levelName(LogLevel level);

}  // namespace theseed::foundation
