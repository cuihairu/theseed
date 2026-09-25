#include "theseed/foundation/Tracing.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <random>
#include <sstream>
#include <stack>
#include <string_view>
#include <utility>

namespace theseed::foundation {

namespace {

constexpr std::string_view kTraceparentKey = "traceparent";
constexpr std::string_view kTraceparentVersion = "00";
constexpr std::string_view kTraceparentFlags = "01";

std::string toHex(std::uint64_t v) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[i] = kHex[v & 0xF];
        v >>= 4;
    }
    return out;
}

std::mutex& rngMutex() {
    static std::mutex m;  // LCOV_EXCL_BR_LINE 函数级 static 初始化守卫边，单线程测试恒走已初始化路径
    return m;
}

std::uint64_t nextRandom64() {
    static std::random_device rd;  // LCOV_EXCL_BR_LINE 函数级 static 初始化守卫边，单线程测试恒走已初始化路径
    static std::mt19937_64 gen{rd()};  // LCOV_EXCL_BR_LINE 同上：static 初始化守卫竞争边不可达
    std::lock_guard lock(rngMutex());
    return gen();
}

std::mutex& emitterMutex() {
    static std::mutex m;  // LCOV_EXCL_BR_LINE 函数级 static 初始化守卫边，单线程测试恒走已初始化路径
    return m;
}

SpanEmitter& emitterRef() {
    static SpanEmitter e;  // LCOV_EXCL_BR_LINE 函数级 static 初始化守卫边，单线程测试恒走已初始化路径
    return e;
}

SpanEmitter currentEmitter() {
    std::lock_guard lock(emitterMutex());
    return emitterRef();
}

std::stack<SpanContext>& threadStack() {
    static thread_local std::stack<SpanContext> s;
    return s;
}

bool isValidHex(std::string_view s, std::size_t expectedLen) {
    if (s.size() != expectedLen) return false;
    for (char c : s) {
        const bool isHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!isHex) return false;
    }
    return true;
}

}  // namespace

bool SpanContext::isValid() const noexcept {
    return isValidHex(traceId, 32) && isValidHex(spanId, 16);
}

SpanContext SpanContext::empty() {
    return SpanContext{};  // LCOV_EXCL_BR_LINE NRVO 关闭路径的返回值拷贝构造边恒不执行（编译器必然消去）
}

SpanScope::SpanScope(std::string name) : span_() {
    span_.name = std::move(name);
    span_.startTime = std::chrono::system_clock::now();
    auto& stack = threadStack();
    if (stack.empty()) {
        span_.context.traceId = generateTraceId();
        span_.context.spanId = generateSpanId();
        span_.context.parentSpanId.clear();
    } else {
        const auto& parent = stack.top();
        span_.context.traceId = parent.traceId;
        span_.context.parentSpanId = parent.spanId;
        span_.context.spanId = generateSpanId();
    }
    stack.push(span_.context);
}

SpanScope::SpanScope(std::string name, SpanContext parent) : span_() {
    span_.name = std::move(name);
    span_.startTime = std::chrono::system_clock::now();
    if (parent.isValid()) {
        span_.context.traceId = parent.traceId;
        span_.context.parentSpanId = parent.spanId;
    } else {
        span_.context.traceId = generateTraceId();
        span_.context.parentSpanId.clear();
    }
    span_.context.spanId = generateSpanId();
    threadStack().push(span_.context);
}

SpanScope::~SpanScope() {
    span_.endTime = std::chrono::system_clock::now();
    auto& stack = threadStack();
    if (!stack.empty()) {
        stack.pop();
    }
    if (auto emit = currentEmitter()) {
        emit(span_);
    }
}

const SpanContext& SpanScope::context() const noexcept {
    return span_.context;
}

const Span& SpanScope::span() const noexcept {
    return span_;
}

void SpanScope::setAttribute(std::string key, LogAttribute::Value value) {
    span_.attrs.push_back(LogAttribute{std::move(key), std::move(value)});
}

SpanContext currentSpanContext() {
    auto& stack = threadStack();
    if (stack.empty()) return SpanContext::empty();
    return stack.top();
}

SpanScope startSpan(std::string name) {
    return SpanScope(std::move(name));
}

SpanScope startChildSpan(std::string name, const SpanContext& parent) {
    return SpanScope(std::move(name), parent);
}

std::string generateTraceId() {
    std::uint64_t hi = nextRandom64();
    std::uint64_t lo = nextRandom64();
    // Avoid the all-zero trace id (invalid per W3C).
    while (hi == 0 && lo == 0) {  // LCOV_EXCL_BR_LINE 真随机下 hi/lo 全零重试概率 2^-64，短路与各臂不可定向构造

        // LCOV_EXCL_START 真随机数生成器下随机 ID 恒全 0 的重试循环不可达
        hi = nextRandom64();
        lo = nextRandom64();
        // LCOV_EXCL_STOP
    }
    return toHex(hi) + toHex(lo);
}

std::string generateSpanId() {
    std::uint64_t v = nextRandom64();
    while (v == 0) v = nextRandom64();  // LCOV_EXCL_BR_LINE 真随机 spanId 全零重试概率 2^-64，不可定向构造

    return toHex(v);
}

void injectContext(const SpanContext& ctx, const ContextSetter& set) {
    if (!set) return;
    if (!ctx.isValid()) return;
    std::ostringstream out;
    out << kTraceparentVersion << '-'
        << ctx.traceId << '-'
        << ctx.spanId << '-'
        << kTraceparentFlags;
    set(kTraceparentKey, out.str());
}

SpanContext extractContext(const ContextGetter& get) {
    if (!get) return SpanContext::empty();
    auto raw = get(kTraceparentKey);
    if (!raw) return SpanContext::empty();

    SpanContext ctx;
    // Parse version-traceid-spanid-flags
    std::istringstream iss(*raw);
    std::string version, traceId, spanId, flags;
    if (!std::getline(iss, version, '-') || version.size() != 2) return SpanContext::empty();
    if (!std::getline(iss, traceId, '-') || traceId.size() != 32) return SpanContext::empty();
    if (!std::getline(iss, spanId, '-') || spanId.size() != 16) return SpanContext::empty();
    if (!std::getline(iss, flags) || flags.size() != 2) return SpanContext::empty();

    ctx.traceId = traceId;
    ctx.spanId = spanId;
    ctx.parentSpanId.clear();
    return ctx;
}

void setSpanEmitter(SpanEmitter emitter) {
    std::lock_guard lock(emitterMutex());
    emitterRef() = std::move(emitter);
}

SpanEmitter takeSpanEmitter() {
    std::lock_guard lock(emitterMutex());
    auto e = emitterRef();
    emitterRef() = SpanEmitter{};
    return e;
}

void resetTracing() {
    auto& stack = threadStack();
    while (!stack.empty()) stack.pop();
    setSpanEmitter(SpanEmitter{});
}

}  // namespace theseed::foundation
