#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace theseed::foundation {

enum class MetricType : std::uint8_t {
    Counter = 0,
    Gauge,
    Histogram,
};

// Lock-free monotonic counter. Thread-safe.
class Counter final {
public:
    Counter() = default;
    void increment(std::uint64_t delta = 1) noexcept;
    std::uint64_t value() const noexcept;

private:
    std::atomic<std::uint64_t> value_{0};
};

// Lock-free gauge holding a signed value. Thread-safe.
class Gauge final {
public:
    Gauge() = default;
    void set(std::int64_t v) noexcept;
    void increment(std::int64_t delta = 1) noexcept;
    void decrement(std::int64_t delta = 1) noexcept;
    std::int64_t value() const noexcept;

private:
    std::atomic<std::int64_t> value_{0};
};

// Histogram with explicit bucket boundaries. Thread-safe via single mutex.
// Bucket counts are cumulative: bucketCounts[i] = #{observations <= boundaries[i]}.
// Last bucket is +Inf.
class Histogram final {
public:
    using Boundaries = std::vector<double>;

    struct Snapshot final {
        std::uint64_t count = 0;
        double sum = 0.0;
        std::vector<std::uint64_t> bucketCounts;
        Boundaries boundaries;
    };

    Histogram() = default;
    explicit Histogram(Boundaries bounds);
    void observe(double value) noexcept;
    Snapshot snapshot() const;

private:
    mutable std::mutex mutex_;
    Boundaries boundaries_;
    std::uint64_t count_ = 0;
    double sum_ = 0.0;
    std::vector<std::uint64_t> bucketCounts_;
};

// Process-wide metric registry. Metrics are addressed by name and return
// stable references; the registry owns their lifetime.
class MetricsRegistry final {
public:
    struct Sample final {
        MetricType type = MetricType::Counter;
        std::string name;
        std::string desc;
        std::variant<std::uint64_t, std::int64_t, Histogram::Snapshot> value;
    };

    static MetricsRegistry& instance();

    Counter& counter(std::string_view name, std::string_view desc = {});
    Gauge& gauge(std::string_view name, std::string_view desc = {});
    Histogram& histogram(std::string_view name,
                         Histogram::Boundaries bounds,
                         std::string_view desc = {});

    std::vector<Sample> collect() const;
    std::string renderText() const;
    std::string renderJson() const;

    // Test-only: drops every metric registered before the call.
    void reset();

    MetricsRegistry(const MetricsRegistry&) = delete;
    MetricsRegistry& operator=(const MetricsRegistry&) = delete;

private:
    struct Entry final {
        MetricType type;
        std::string desc;
        std::unique_ptr<Counter> counter;
        std::unique_ptr<Gauge> gauge;
        std::unique_ptr<Histogram> histogram;
    };

    MetricsRegistry();
    ~MetricsRegistry() = default;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<Entry>> entries_;
    std::vector<std::string> order_;
};

}  // namespace theseed::foundation
