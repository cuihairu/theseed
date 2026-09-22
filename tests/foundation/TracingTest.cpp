#include "theseed/foundation/Tracing.h"
#include "theseed/foundation/Logger.h"

#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using theseed::foundation::Span;
using theseed::foundation::SpanContext;
using theseed::foundation::SpanScope;
using theseed::foundation::currentSpanContext;
using theseed::foundation::extractContext;
using theseed::foundation::generateSpanId;
using theseed::foundation::generateTraceId;
using theseed::foundation::injectContext;
using theseed::foundation::LogAttribute;
using theseed::foundation::resetTracing;
using theseed::foundation::setSpanEmitter;
using theseed::foundation::startChildSpan;
using theseed::foundation::startSpan;
using theseed::foundation::takeSpanEmitter;

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
        std::cout << "FAIL: " << (msg) << "\n";                 \
        ++testsFailed;                                          \
    } while (0)

static void test_generate_trace_id_length() {
    TEST("test_generate_trace_id_length");
    const auto id = generateTraceId();
    if (id.size() != 32) { FAIL("trace id must be 32 hex chars"); return; }
    for (char c : id) {
        const bool isHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!isHex) { FAIL("trace id must be lowercase hex"); return; }
    }
    PASS();
}

static void test_generate_span_id_length() {
    TEST("test_generate_span_id_length");
    const auto id = generateSpanId();
    if (id.size() != 16) { FAIL("span id must be 16 hex chars"); return; }
    PASS();
}

static void test_generate_trace_id_is_unique() {
    TEST("test_generate_trace_id_is_unique");
    const auto a = generateTraceId();
    const auto b = generateTraceId();
    if (a == b) { FAIL("two consecutive trace ids equal"); return; }
    PASS();
}

static void test_span_context_empty_invalid() {
    TEST("test_span_context_empty_invalid");
    const auto e = SpanContext::empty();
    if (e.isValid()) { FAIL("empty context should be invalid"); return; }
    if (!e.traceId.empty() || !e.spanId.empty()) { FAIL("empty context should be all empty"); return; }
    PASS();
}

static void test_start_span_creates_root() {
    TEST("test_start_span_creates_root");
    resetTracing();
    auto scope = startSpan("root");
    const auto ctx = scope.context();
    if (ctx.traceId.size() != 32) { FAIL("traceId size wrong"); return; }
    if (ctx.spanId.size() != 16) { FAIL("spanId size wrong"); return; }
    if (!ctx.parentSpanId.empty()) { FAIL("root should have empty parent"); return; }
    if (!ctx.isValid()) { FAIL("context should be valid"); return; }
    PASS();
}

static void test_current_span_context_tracks_stack() {
    TEST("test_current_span_context_tracks_stack");
    resetTracing();
    if (currentSpanContext().isValid()) { FAIL("expected empty before scope"); return; }
    {
        auto parent = startSpan("parent");
        const auto ctx1 = currentSpanContext();
        if (ctx1.spanId != parent.context().spanId) { FAIL("current does not match parent"); return; }
        {
            auto child = startSpan("child");
            const auto ctx2 = currentSpanContext();
            if (ctx2.spanId != child.context().spanId) { FAIL("current does not match child"); return; }
            if (ctx2.parentSpanId != parent.context().spanId) { FAIL("child parent wrong"); return; }
            if (ctx2.traceId != parent.context().traceId) { FAIL("child trace id diverged"); return; }
        }
        const auto ctx3 = currentSpanContext();
        if (ctx3.spanId != parent.context().spanId) { FAIL("current not restored after child scope"); return; }
    }
    if (currentSpanContext().isValid()) { FAIL("expected empty after scope exit"); return; }
    PASS();
}

static void test_start_child_span_with_external_parent() {
    TEST("test_start_child_span_with_external_parent");
    resetTracing();
    SpanContext external;
    external.traceId = generateTraceId();
    external.spanId = generateSpanId();
    auto scope = startChildSpan("ingress", external);
    const auto ctx = scope.context();
    if (ctx.traceId != external.traceId) { FAIL("child did not inherit traceId"); return; }
    if (ctx.parentSpanId != external.spanId) { FAIL("child parent should be external"); return; }
    PASS();
}

static void test_start_child_span_with_empty_parent_is_root() {
    TEST("test_start_child_span_with_empty_parent_is_root");
    resetTracing();
    auto scope = startChildSpan("root", SpanContext::empty());
    const auto ctx = scope.context();
    if (!ctx.parentSpanId.empty()) { FAIL("should be root"); return; }
    PASS();
}

static void test_emit_called_on_destruction() {
    TEST("test_emit_called_on_destruction");
    resetTracing();
    int emitCount = 0;
    Span lastSpan;
    setSpanEmitter([&](const Span& s) {
        ++emitCount;
        lastSpan = s;
    });
    {
        auto scope = startSpan("emit-test");
        scope.setAttribute("kind", std::string("test"));
    }
    if (emitCount != 1) { FAIL("emit not called exactly once"); return; }
    if (lastSpan.name != "emit-test") { FAIL("span name wrong"); return; }
    if (lastSpan.attrs.size() != 1) { FAIL("attrs not preserved"); return; }
    if (lastSpan.endTime <= lastSpan.startTime) { FAIL("endTime not set"); return; }
    resetTracing();
    PASS();
}

static void test_emit_can_be_disabled() {
    TEST("test_emit_can_be_disabled");
    resetTracing();
    int emitCount = 0;
    setSpanEmitter([&](const Span&) { ++emitCount; });
    takeSpanEmitter();
    {
        auto scope = startSpan("no-emit");
        static_cast<void>(scope);
    }
    if (emitCount != 0) { FAIL("emit fired after takeSpanEmitter"); return; }
    PASS();
}

static void test_inject_writes_traceparent() {
    TEST("test_inject_writes_traceparent");
    resetTracing();
    SpanContext ctx;
    ctx.traceId = generateTraceId();
    ctx.spanId = generateSpanId();
    std::map<std::string, std::string> headers;
    injectContext(ctx, [&](std::string_view k, std::string_view v) {
        headers[std::string(k)] = std::string(v);
    });
    if (headers.find("traceparent") == headers.end()) { FAIL("traceparent missing"); return; }
    const auto& tp = headers["traceparent"];
    if (tp.size() != 55) { FAIL("traceparent must be 55 chars"); return; }
    if (tp.substr(0, 3) != "00-") { FAIL("version prefix wrong"); return; }
    if (tp.find(ctx.traceId) == std::string::npos) { FAIL("traceId not embedded"); return; }
    if (tp.find(ctx.spanId) == std::string::npos) { FAIL("spanId not embedded"); return; }
    PASS();
}

static void test_extract_round_trips() {
    TEST("test_extract_round_trips");
    resetTracing();
    SpanContext ctx;
    ctx.traceId = generateTraceId();
    ctx.spanId = generateSpanId();
    std::map<std::string, std::string> headers;
    injectContext(ctx, [&](std::string_view k, std::string_view v) {
        headers[std::string(k)] = std::string(v);
    });
    auto extracted = extractContext([&](std::string_view k) -> std::optional<std::string> {
        auto it = headers.find(std::string(k));
        if (it == headers.end()) return std::nullopt;
        return it->second;
    });
    if (!extracted.isValid()) { FAIL("extracted context not valid"); return; }
    if (extracted.traceId != ctx.traceId) { FAIL("traceId mismatch"); return; }
    if (extracted.spanId != ctx.spanId) { FAIL("spanId mismatch"); return; }
    PASS();
}

static void test_extract_rejects_malformed() {
    TEST("test_extract_rejects_malformed");
    std::map<std::string, std::string> headers;
    headers["traceparent"] = "garbage";
    auto extracted = extractContext([&](std::string_view k) -> std::optional<std::string> {
        auto it = headers.find(std::string(k));
        if (it == headers.end()) return std::nullopt;
        return it->second;
    });
    if (extracted.isValid()) { FAIL("malformed input should yield invalid ctx"); return; }
    PASS();
}

static void test_inject_invalid_context_skips() {
    TEST("test_inject_invalid_context_skips");
    int calls = 0;
    injectContext(SpanContext::empty(), [&](std::string_view, std::string_view) {
        ++calls;
    });
    if (calls != 0) { FAIL("invalid ctx should not be injected"); return; }
    PASS();
}

static void test_thread_isolation() {
    TEST("test_thread_isolation");
    resetTracing();
    auto scope = startSpan("main-thread");
    const auto mainSpanId = scope.context().spanId;
    const auto mainTraceId = scope.context().traceId;

    std::string otherSpanId;
    std::string otherCurrent;
    std::thread t([&] {
        // On a fresh thread, the current span stack must be empty.
        otherCurrent = currentSpanContext().isValid() ? "valid" : "empty";
        auto child = startSpan("other-thread");
        otherSpanId = child.context().spanId;
    });
    t.join();

    if (otherCurrent != "empty") { FAIL("new thread should have empty stack"); return; }
    if (otherSpanId.empty()) { FAIL("other thread failed to create span"); return; }
    if (otherSpanId == mainSpanId) { FAIL("span ids collided across threads"); return; }

    // Main-thread context still intact.
    if (currentSpanContext().spanId != mainSpanId) { FAIL("main thread stack corrupted"); return; }
    static_cast<void>(mainTraceId);
    PASS();
}

class CapturingLogger final : public theseed::foundation::ILogger {
public:
    void log(theseed::foundation::LogRecord record) override {
        const std::lock_guard lock(mtx_);
        records_.push_back(std::move(record));
    }
    void setLevel(theseed::foundation::LogLevel l) override { level_ = l; }
    theseed::foundation::LogLevel level() const override { return level_; }

    std::vector<theseed::foundation::LogRecord> drain() {
        const std::lock_guard lock(mtx_);
        auto out = std::move(records_);
        records_.clear();
        return out;
    }

private:
    theseed::foundation::LogLevel level_ = theseed::foundation::LogLevel::Debug;
    std::mutex mtx_;
    std::vector<theseed::foundation::LogRecord> records_;
};

static void test_logger_auto_attaches_trace() {
    TEST("test_logger_auto_attaches_trace");
    resetTracing();
    auto capturer = std::make_shared<CapturingLogger>();
    auto prev = theseed::foundation::takeGlobalLogger();
    theseed::foundation::setGlobalLogger(capturer);

    // Outside a span: no trace context.
    theseed::foundation::logInfo("outside");
    // Inside a span: trace should be attached.
    std::string expectedTrace;
    std::string expectedSpan;
    {
        auto scope = startSpan("logged-span");
        expectedTrace = scope.context().traceId;
        expectedSpan = scope.context().spanId;
        theseed::foundation::logInfo("inside");
    }

    auto records = capturer->drain();
    theseed::foundation::setGlobalLogger(prev);

    if (records.size() != 2) { FAIL("expected 2 records"); return; }
    if (records[0].traceId.has_value()) { FAIL("outside log should have no traceId"); return; }
    if (records[1].traceId.value_or("") != expectedTrace) { FAIL("inside log traceId mismatch"); return; }
    if (records[1].spanId.value_or("") != expectedSpan) { FAIL("inside log spanId mismatch"); return; }
    PASS();
}

int main() {
    test_generate_trace_id_length();
    test_generate_span_id_length();
    test_generate_trace_id_is_unique();
    test_span_context_empty_invalid();
    test_start_span_creates_root();
    test_current_span_context_tracks_stack();
    test_start_child_span_with_external_parent();
    test_start_child_span_with_empty_parent_is_root();
    test_emit_called_on_destruction();
    test_emit_can_be_disabled();
    test_inject_writes_traceparent();
    test_extract_round_trips();
    test_extract_rejects_malformed();
    test_inject_invalid_context_skips();
    test_thread_isolation();
    test_logger_auto_attaches_trace();

    std::cout << "  passed=" << testsPassed << " failed=" << testsFailed << "\n";
    return testsFailed == 0 ? 0 : 1;
}
