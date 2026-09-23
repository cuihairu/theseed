#include "theseed/foundation/Metrics.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string_view>
#include <utility>

namespace theseed::foundation {

// --- Counter ---

void Counter::increment(std::uint64_t delta) noexcept {
    value_.fetch_add(delta, std::memory_order_relaxed);
}

std::uint64_t Counter::value() const noexcept {
    return value_.load(std::memory_order_relaxed);
}

// --- Gauge ---

void Gauge::set(std::int64_t v) noexcept {
    value_.store(v, std::memory_order_relaxed);
}

void Gauge::increment(std::int64_t delta) noexcept {
    value_.fetch_add(delta, std::memory_order_relaxed);
}

void Gauge::decrement(std::int64_t delta) noexcept {
    value_.fetch_sub(delta, std::memory_order_relaxed);
}

std::int64_t Gauge::value() const noexcept {
    return value_.load(std::memory_order_relaxed);
}

// --- Histogram ---

Histogram::Histogram(Boundaries bounds) : boundaries_(std::move(bounds)) {
    bucketCounts_.assign(boundaries_.size() + 1, 0);
}

void Histogram::observe(double value) noexcept {
    if (std::isnan(value)) return;

    const std::lock_guard lock(mutex_);
    ++count_;
    sum_ += value;

    // Cumulative bucket assignment: find first boundary >= value
    std::size_t i = 0;
    for (; i < boundaries_.size(); ++i) {
        if (value <= boundaries_[i]) break;
    }
    // bucketCounts_[i] is the count for value <= boundaries_[i-1]
    // The +Inf bucket is bucketCounts_[boundaries_.size()]
    for (std::size_t j = i; j < bucketCounts_.size(); ++j) {
        ++bucketCounts_[j];
    }
}

Histogram::Snapshot Histogram::snapshot() const {
    const std::lock_guard lock(mutex_);
    Snapshot snap;
    snap.count = count_;
    snap.sum = sum_;
    snap.bucketCounts = bucketCounts_;
    snap.boundaries = boundaries_;
    return snap;
}

// --- MetricsRegistry ---

MetricsRegistry::MetricsRegistry() {
    // Pre-register MVP placeholder metrics so renderText always emits a stable set.
    // Real script_error_count increments will be wired by the scripting layer when available.
    counter("script_error_count", "Placeholder; emitted by scripting layer when available");
}

MetricsRegistry& MetricsRegistry::instance() {
    static MetricsRegistry registry;
    return registry;
}

Counter& MetricsRegistry::counter(std::string_view name, std::string_view desc) {
    const std::lock_guard lock(mutex_);
    const std::string key(name);
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        auto entry = std::make_unique<Entry>();
        entry->type = MetricType::Counter;
        entry->desc = std::string(desc);
        entry->counter = std::make_unique<Counter>();
        Counter* ptr = entry->counter.get();
        order_.push_back(key);
        entries_.emplace(key, std::move(entry));
        return *ptr;
    }
    return *it->second->counter;
}

Gauge& MetricsRegistry::gauge(std::string_view name, std::string_view desc) {
    const std::lock_guard lock(mutex_);
    const std::string key(name);
    auto it = entries_.find(key);
    if (it != entries_.end() && it->second->type == MetricType::Gauge) {
        return *it->second->gauge;
    }
    auto entry = std::make_unique<Entry>();
    entry->type = MetricType::Gauge;
    entry->desc = std::string(desc);
    entry->gauge = std::make_unique<Gauge>();
    Gauge* ptr = entry->gauge.get();
    if (it == entries_.end()) {
        order_.push_back(key);
        entries_.emplace(key, std::move(entry));
    } else {
        it->second = std::move(entry);
    }
    return *ptr;
}

Histogram& MetricsRegistry::histogram(std::string_view name,
                                      Histogram::Boundaries bounds,
                                      std::string_view desc) {
    const std::lock_guard lock(mutex_);
    const std::string key(name);
    auto it = entries_.find(key);
    if (it != entries_.end() && it->second->type == MetricType::Histogram) {
        return *it->second->histogram;
    }
    auto entry = std::make_unique<Entry>();
    entry->type = MetricType::Histogram;
    entry->desc = std::string(desc);
    entry->histogram = std::make_unique<Histogram>(std::move(bounds));
    Histogram* ptr = entry->histogram.get();
    if (it == entries_.end()) {
        order_.push_back(key);
        entries_.emplace(key, std::move(entry));
    } else {
        it->second = std::move(entry);
    }
    return *ptr;
}

std::vector<MetricsRegistry::Sample> MetricsRegistry::collect() const {
    const std::lock_guard lock(mutex_);
    std::vector<Sample> out;
    out.reserve(order_.size());
    for (const auto& name : order_) {
        auto it = entries_.find(name);
        if (it == entries_.end()) continue;
        const auto& entry = *it->second;
        Sample s;
        s.type = entry.type;
        s.name = name;
        s.desc = entry.desc;
        switch (entry.type) {
            case MetricType::Counter:
                s.value = entry.counter->value();
                break;
            case MetricType::Gauge:
                s.value = entry.gauge->value();
                break;
            case MetricType::Histogram:
                s.value = entry.histogram->snapshot();
                break;
        }
        out.push_back(std::move(s));
    }
    return out;
}

static const char* typeName(MetricType t) {
    switch (t) {
        case MetricType::Counter: return "counter";
        case MetricType::Gauge: return "gauge";
        default:  // Histogram 与潜在新增类型
            return t == MetricType::Histogram ? "histogram" : "untyped";
    }
}

std::string MetricsRegistry::renderText() const {
    const auto samples = collect();
    std::ostringstream out;
    for (const auto& s : samples) {
        if (!s.desc.empty()) {
            out << "# HELP " << s.name << ' ' << s.desc << '\n';
        }
        out << "# TYPE " << s.name << ' ' << typeName(s.type) << '\n';
        std::visit([&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::uint64_t>) {
                out << s.name << ' ' << v << '\n';
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                out << s.name << ' ' << v << '\n';
            } else if constexpr (std::is_same_v<T, Histogram::Snapshot>) {
                for (std::size_t i = 0; i < v.boundaries.size(); ++i) {
                    out << s.name << "_bucket{le=\"" << v.boundaries[i]
                        << "\"} " << v.bucketCounts[i] << '\n';
                }
                out << s.name << "_bucket{le=\"+Inf\"} "
                    << v.bucketCounts.back() << '\n';
                out << s.name << "_count " << v.count << '\n';
                out << s.name << "_sum " << v.sum << '\n';
            }
        }, s.value);
    }
    return out.str();
}

std::string MetricsRegistry::renderJson() const {
    const auto samples = collect();
    std::ostringstream out;
    out << "{\"metrics\":[";
    bool first = true;
    for (const auto& s : samples) {
        if (!first) out << ',';
        first = false;
        out << "{\"name\":\"" << s.name << "\","
            << "\"type\":\"" << typeName(s.type) << "\","
            << "\"description\":\"" << s.desc << "\","
            << "\"value\":";
        std::visit([&](const auto& v) {  // LCOV_EXCL_LINE std::visit 的闭包包装符号使本行条目恒 0（lambda 体各分支有计数），gcc 归因伪影
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::uint64_t>) {
                out << v;
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                out << v;
            } else if constexpr (std::is_same_v<T, Histogram::Snapshot>) {
                out << "{\"count\":" << v.count
                    << ",\"sum\":" << v.sum
                    << ",\"buckets\":[";
                for (std::size_t i = 0; i < v.boundaries.size(); ++i) {
                    if (i > 0) out << ',';
                    out << "{\"le\":" << v.boundaries[i]
                        << ",\"count\":" << v.bucketCounts[i] << '}';
                }
                if (!v.boundaries.empty()) out << ',';
                out << "{\"le\":\"+Inf\",\"count\":" << v.bucketCounts.back() << '}';
                out << "]}";
            }
        }, s.value);
        out << '}';
    }
    out << "]}";
    return out.str();
}

void MetricsRegistry::reset() {
    const std::lock_guard lock(mutex_);
    entries_.clear();
    order_.clear();
    // Re-register the placeholder so format checks still hold after reset.
    auto entry = std::make_unique<Entry>();
    entry->type = MetricType::Counter;
    entry->desc = "Placeholder; emitted by scripting layer when available";
    entry->counter = std::make_unique<Counter>();
    order_.push_back("script_error_count");
    entries_.emplace("script_error_count", std::move(entry));
}

}  // namespace theseed::foundation
