#pragma once

#include "theseed/foundation/Logger.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace theseed::foundation {

// Minimal W3C-style trace context for cross-process propagation.
//
// traceId    — 32 hex chars (128 bits)
// spanId     — 16 hex chars (64 bits)
// parentSpanId — 16 hex chars or empty for a root span
//
// MVP scope: thread-local span stack + inject/extract via generic carrier
// callbacks. No OTel SDK dependency — emit hooks let tests / future OTel
// exporters consume finished spans.
struct SpanContext final {
    std::string traceId;
    std::string spanId;
    std::string parentSpanId;

    bool isValid() const noexcept;
    static SpanContext empty();
};

struct Span final {
    std::string name;
    SpanContext context;
    std::chrono::system_clock::time_point startTime;
    std::chrono::system_clock::time_point endTime;
    std::vector<LogAttribute> attrs;
};

// RAII scope that pushes a new span onto the current thread's stack and
// pops/emits on destruction. Move-disabled; construct as an automatic
// variable or unique_ptr.
class SpanScope final {
public:
    SpanScope(std::string name, SpanContext parent);
    SpanScope(std::string name);  // child of current, or root if none
    ~SpanScope();

    SpanScope(const SpanScope&) = delete;
    SpanScope& operator=(const SpanScope&) = delete;
    SpanScope(SpanScope&&) = delete;
    SpanScope& operator=(SpanScope&&) = delete;

    const SpanContext& context() const noexcept;
    const Span& span() const noexcept;
    void setAttribute(std::string key, LogAttribute::Value value);

private:
    Span span_;
};

// Thread-local current span context (empty if no active SpanScope on this thread).
SpanContext currentSpanContext();

// Convenience constructors. startChildSpan with empty parent == root.
SpanScope startSpan(std::string name);
SpanScope startChildSpan(std::string name, const SpanContext& parent);

// New random IDs (used internally and exposed for testing).
std::string generateTraceId();
std::string generateSpanId();

// Carrier-style propagation, decoupled from runtime/Bundle.
// W3C traceparent format: version-traceid-spanid-flags
using ContextSetter = std::function<void(std::string_view key, std::string_view value)>;
using ContextGetter = std::function<std::optional<std::string>(std::string_view key)>;

void injectContext(const SpanContext& ctx, const ContextSetter& set);
SpanContext extractContext(const ContextGetter& get);

// Emit hook — invoked once per finished Span. nullptr is allowed and means
// "drop". Default state is a no-op emitter so production builds stay quiet.
using SpanEmitter = std::function<void(const Span&)>;
void setSpanEmitter(SpanEmitter emitter);
SpanEmitter takeSpanEmitter();

// Test helper: clears thread-local span stack and resets the emitter.
void resetTracing();

}  // namespace theseed::foundation
