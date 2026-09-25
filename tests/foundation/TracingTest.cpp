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

// isValidHex 三段短路链的各假臂：按字符类逐一构造非法 traceId。
static void test_is_valid_hex_character_classes() {
    TEST("test_is_valid_hex_character_classes");
    const char badChars[] = {'/', ':', '@', '`', 'g', '\x01'};
    for (const char bad : badChars) {
        SpanContext ctx;
        ctx.traceId = std::string(32, '0');
        ctx.traceId[31] = bad;  // 末位换成非法字符，循环走完整链后中断
        ctx.spanId = std::string(16, 'a');
        if (ctx.isValid()) {
            FAIL(std::string("character should be invalid: ") + bad);
            return;
        }
    }
    // 大写 A-F 合法（第三段短路臂的路径）
    SpanContext upper;
    upper.traceId = std::string(32, 'F');
    upper.spanId = std::string(16, 'A');
    if (!upper.isValid()) { FAIL("uppercase hex should be valid"); return; }
    // traceId 长度错误：isValid 第一段短路，不再评估 spanId
    SpanContext shortTrace;
    shortTrace.traceId = std::string(31, '0');
    shortTrace.spanId = std::string(16, 'a');
    if (shortTrace.isValid()) { FAIL("wrong trace length should be invalid"); return; }
    // traceId 合法、spanId 长度错误：第二段短路臂
    SpanContext shortSpan;
    shortSpan.traceId = std::string(32, '0');
    shortSpan.spanId = std::string(17, 'a');
    if (shortSpan.isValid()) { FAIL("wrong span length should be invalid"); return; }
    PASS();
}

static void test_inject_extract_defensive_shortcuts() {
    TEST("test_inject_extract_defensive_shortcuts");
    SpanContext valid;
    valid.traceId = generateTraceId();
    valid.spanId = generateSpanId();
    // 空 setter：injectContext 直接返回，不产生调用
    int setterCalls = 0;
    injectContext(valid, [&](std::string_view, std::string_view) { ++setterCalls; });
    if (setterCalls != 1) { FAIL("valid inject should call setter once"); return; }
    injectContext(valid, nullptr);
    if (setterCalls != 1) { FAIL("null setter should be skipped"); return; }
    // 空 getter / 缺键：extractContext 返回无效上下文
    auto noGetter = extractContext(nullptr);
    if (noGetter.isValid()) { FAIL("null getter should yield invalid ctx"); return; }
    auto missing = extractContext([](std::string_view) -> std::optional<std::string> {
        return std::nullopt;
    });
    if (missing.isValid()) { FAIL("missing header should yield invalid ctx"); return; }
    PASS();
}

// traceparent 解析链（getline + 长度校验）各失败臂的畸形输入矩阵。
static void test_extract_malformed_matrix() {
    TEST("test_extract_malformed_matrix");
    const std::string trace32(32, '5');
    const std::string span16(16, '8');
    const std::string bad[] = {
        "",                                 // 空串：首段 getline 直接失败
        "0-x",                              // version 长度 1
        "00",                               // traceId 段缺失
        "00-" + trace32 + "0-" + span16 + "-ff",  // traceId 33 位
        "00-" + trace32 + "-abc-ff",        // spanId 3 位
        "00-" + trace32 + "-",              // spanId 段缺失：getline 失败
        "00-" + trace32 + "-" + span16,     // flags 段缺失
        "00-" + trace32 + "-" + span16 + "-xyz",  // flags 3 位
        "zz-" + trace32 + "-" + span16 + "-ff",   // version 内容不影响（仅长度）
    };
    for (const auto& raw : bad) {
        auto ctx = extractContext([&raw](std::string_view) -> std::optional<std::string> {
            return raw;
        });
        if (raw.substr(0, 2) == "zz") {
            // version 只查长度不查内容：此条应解析成功
            if (!ctx.isValid()) { FAIL("version content should not matter"); return; }
            continue;
        }
        if (ctx.isValid()) { FAIL("malformed traceparent should be rejected: " + raw); return; }
    }
    PASS();
}

// resetTracing 时栈非空：析构在 reset 后发生，覆盖析构的空栈臂与 reset 的 pop 循环体。
static void test_scope_destroy_after_reset_pops_stack() {
    TEST("test_scope_destroy_after_reset_pops_stack");
    resetTracing();
    auto* scope = new auto(startSpan("leaked-then-reset"));
    const auto spanId = scope->context().spanId;
    resetTracing();  // 栈内仍有该 span：while 循环体执行 pop
    if (currentSpanContext().isValid()) { FAIL("stack should be empty after reset"); return; }
    delete scope;  // 析构时栈已空：!stack.empty() 假臂；无 emitter → 不发射
    if (spanId.empty()) { FAIL("span id should have been captured"); return; }
    PASS();
}

// 同一 span 追加多个属性：attrs 向量经历 0→1 增长与 1→2 重分配两条分支。
static void test_set_attribute_growth() {
    TEST("test_set_attribute_growth");
    resetTracing();
    int emitCount = 0;
    Span lastSpan;
    setSpanEmitter([&](const Span& s) {
        ++emitCount;
        lastSpan = s;
    });
    {
        auto scope = startSpan("attrs-growth");
        scope.setAttribute("first", std::string("1"));
        scope.setAttribute("second", std::string("2"));
        scope.setAttribute("third", std::int64_t{3});
        scope.setAttribute("fourth", std::string("4"));  // 触发 2→4 重分配
    }
    if (emitCount != 1) { FAIL("emit not called once"); return; }
    if (lastSpan.attrs.size() != 4) { FAIL("expected 4 attrs"); return; }
    resetTracing();
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

static void test_span_accessor_returns_scope_span() {
    TEST("test_span_accessor_returns_scope_span");
    resetTracing();
    {
        auto scope = startSpan("accessor");
        const auto& span = scope.span();
        if (span.context.spanId != scope.context().spanId) {
            FAIL("span() should expose the scope's own span");
            return;
        }
    }
    PASS();
}

int main() {
    test_generate_trace_id_length();
    test_generate_span_id_length();
    test_generate_trace_id_is_unique();
    test_span_context_empty_invalid();
    test_is_valid_hex_character_classes();
    test_start_span_creates_root();
    test_current_span_context_tracks_stack();
    test_start_child_span_with_external_parent();
    test_start_child_span_with_empty_parent_is_root();
    test_emit_called_on_destruction();
    test_emit_can_be_disabled();
    test_inject_writes_traceparent();
    test_inject_extract_defensive_shortcuts();
    test_extract_round_trips();
    test_extract_malformed_matrix();
    test_extract_rejects_malformed();
    test_inject_invalid_context_skips();
    test_scope_destroy_after_reset_pops_stack();
    test_set_attribute_growth();
    test_thread_isolation();
    test_logger_auto_attaches_trace();
    test_span_accessor_returns_scope_span();

    std::cout << "  passed=" << testsPassed << " failed=" << testsFailed << "\n";
    return testsFailed == 0 ? 0 : 1;
}
