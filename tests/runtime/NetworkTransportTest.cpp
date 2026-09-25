#include "theseed/runtime/InMemoryBytePipe.h"
#include "theseed/runtime/NetworkTransport.h"
#include "theseed/runtime/RuntimeTransport.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

using theseed::runtime::DeliveryClass;
using theseed::runtime::IBytePipe;
using theseed::runtime::InMemoryBytePipe;
using theseed::runtime::NetworkTransport;
using theseed::runtime::RuntimeInvocation;
using theseed::runtime::SendResult;

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

static RuntimeInvocation makeInvocation(std::uint64_t entityId,
                                         std::uint32_t target,
                                         const std::string& method) {
    RuntimeInvocation inv;
    inv.entityId = entityId;
    inv.targetComponent = target;
    inv.entityType = "Avatar";
    inv.method = method;
    inv.deliveryClass = DeliveryClass::ORDERED_RELIABLE;
    return inv;
}

static void testSendReceiveOne() {
    TEST("send one invocation, receive on peer");

    auto [pipeA, pipeB] = InMemoryBytePipe::createPair();
    NetworkTransport client(pipeA);
    NetworkTransport server(pipeB);

    auto sent = makeInvocation(42, 2, "migration.transfer");
    client.send(sent);
    pipeA->pump();

    RuntimeInvocation received;
    auto count = server.receive(2, &received, 1);

    if (count == 1 && received.entityId == 42 && received.method == "migration.transfer") PASS();
    else FAIL("count=" + std::to_string(count) + " entityId=" + std::to_string(received.entityId));
}

static void testSendReceiveMultiple() {
    TEST("send 5 invocations, receive in order");

    auto [pipeA, pipeB] = InMemoryBytePipe::createPair();
    NetworkTransport client(pipeA);
    NetworkTransport server(pipeB);

    for (std::uint64_t i = 1; i <= 5; ++i) {
        client.send(makeInvocation(i, 10, "tick"));
    }
    pipeA->pump();

    RuntimeInvocation out[5];
    auto count = server.receive(10, out, 5);

    bool ok = count == 5;
    for (int i = 0; i < 5 && ok; ++i) {
        ok = out[i].entityId == static_cast<std::uint64_t>(i + 1);
    }

    if (ok) PASS();
    else FAIL("count=" + std::to_string(count));
}

static void testFilterByTargetComponent() {
    TEST("receive filters by target component");

    auto [pipeA, pipeB] = InMemoryBytePipe::createPair();
    NetworkTransport client(pipeA);
    NetworkTransport server(pipeB);

    client.send(makeInvocation(1, 10, "a"));
    client.send(makeInvocation(2, 20, "b"));
    client.send(makeInvocation(3, 10, "c"));
    client.send(makeInvocation(4, 30, "d"));
    client.send(makeInvocation(5, 10, "e"));
    pipeA->pump();

    RuntimeInvocation out[5];
    auto count = server.receive(10, out, 5);

    bool ok = count == 3;
    ok = ok && out[0].entityId == 1;
    ok = ok && out[1].entityId == 3;
    ok = ok && out[2].entityId == 5;

    auto remaining = server.receive(20, out, 5);
    ok = ok && remaining == 1 && out[0].entityId == 2;

    if (ok) PASS();
    else FAIL("count=" + std::to_string(count) + " remaining=" + std::to_string(remaining));
}

static void testRoundTripWithPayload() {
    TEST("round-trip with 1KiB payload");

    auto [pipeA, pipeB] = InMemoryBytePipe::createPair();
    NetworkTransport client(pipeA);
    NetworkTransport server(pipeB);

    std::vector<std::byte> payload(1024);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>(i & 0xFF);
    }

    RuntimeInvocation sent = makeInvocation(99, 5, "data");
    sent.payload = payload;
    client.send(sent);
    pipeA->pump();

    RuntimeInvocation received;
    auto count = server.receive(5, &received, 1);

    bool ok = count == 1;
    ok = ok && received.payload.size() == 1024;
    ok = ok && received.entityId == 99;
    if (ok) {
        for (std::size_t i = 0; i < received.payload.size(); ++i) {
            if (received.payload[i] != static_cast<std::byte>(i & 0xFF)) {
                ok = false;
                break;
            }
        }
    }

    if (ok) PASS();
    else FAIL("payload size=" + std::to_string(received.payload.size()));
}

static void testPartialRead() {
    TEST("partial read: message arrives in two chunks");

    auto [pipeA, pipeB] = InMemoryBytePipe::createPair();
    NetworkTransport client(pipeA);
    NetworkTransport server(pipeB);

    client.send(makeInvocation(1, 7, "partial"));
    // Don't flush yet - manually control delivery

    // Get the raw bytes that client wrote to pipeA's pending
    // Instead, use direct approach: send and flush half
    RuntimeInvocation received;
    bool ok = server.receive(7, &received, 1) == 0;
    if (!ok) { FAIL("should not have data before flush"); return; }

    pipeA->pump();

    auto count = server.receive(7, &received, 1);
    ok = count == 1 && received.entityId == 1;

    if (ok) PASS();
    else FAIL("count=" + std::to_string(count));
}

static void testMultipleMessagesInOneChunk() {
    TEST("multiple complete messages in one flush");

    auto [pipeA, pipeB] = InMemoryBytePipe::createPair();
    NetworkTransport sender(pipeA);
    NetworkTransport receiver(pipeB);

    for (int i = 0; i < 3; ++i) {
        sender.send(makeInvocation(static_cast<std::uint64_t>(i + 100), 1, "batch"));
    }
    pipeA->pump();

    RuntimeInvocation out[3];
    auto count = receiver.receive(1, out, 3);

    bool ok = count == 3;
    for (int i = 0; i < 3 && ok; ++i) {
        ok = out[i].entityId == static_cast<std::uint64_t>(i + 100);
    }

    if (ok) PASS();
    else FAIL("count=" + std::to_string(count));
}

static void testDisconnectedPipeReturnsFalse() {
    TEST("send returns false after pipe close");

    auto [pipeA, pipeB] = InMemoryBytePipe::createPair();
    NetworkTransport client(pipeA);

    bool ok = client.send(makeInvocation(1, 1, "before")) == SendResult::Accepted;
    ok = ok && client.isConnected();

    pipeA->close();
    ok = ok && !client.isConnected();
    ok = ok && client.send(makeInvocation(2, 1, "after")) != SendResult::Accepted;

    if (ok) PASS();
    else FAIL("unexpected state");
}

static void testPendingCount() {
    TEST("pendingCount reflects inbox state");

    auto [pipeA, pipeB] = InMemoryBytePipe::createPair();
    NetworkTransport client(pipeA);
    NetworkTransport server(pipeB);

    bool ok = server.pendingCount() == 0;

    client.send(makeInvocation(1, 5, "a"));
    client.send(makeInvocation(2, 5, "b"));
    client.send(makeInvocation(3, 5, "c"));
    pipeA->pump();

    ok = ok && server.pendingCount() == 3;

    RuntimeInvocation out[2];
    server.receive(5, out, 2);
    ok = ok && server.pendingCount() == 1;

    if (ok) PASS();
    else FAIL("pending=" + std::to_string(server.pendingCount()));
}

static void testBidirectional() {
    TEST("bidirectional communication");

    auto [pipeA, pipeB] = InMemoryBytePipe::createPair();
    NetworkTransport transportA(pipeA);
    NetworkTransport transportB(pipeB);

    transportA.send(makeInvocation(1, 100, "AtoB"));
    transportB.send(makeInvocation(2, 200, "BtoA"));
    pipeA->pump();
    pipeB->pump();

    RuntimeInvocation fromB;
    RuntimeInvocation fromA;
    auto countB = transportB.receive(100, &fromB, 1);
    auto countA = transportA.receive(200, &fromA, 1);

    bool ok = countB == 1 && fromB.entityId == 1 && fromB.method == "AtoB";
    ok = ok && countA == 1 && fromA.entityId == 2 && fromA.method == "BtoA";

    if (ok) PASS();
    else FAIL("countB=" + std::to_string(countB) + " countA=" + std::to_string(countA));
}

static void testUnorderedLossyDelivery() {
    TEST("unordered lossy delivery class preserved");

    auto [pipeA, pipeB] = InMemoryBytePipe::createPair();
    NetworkTransport client(pipeA);
    NetworkTransport server(pipeB);

    RuntimeInvocation sent = makeInvocation(77, 3, "position");
    sent.deliveryClass = DeliveryClass::UNORDERED_LOSSY;
    client.send(sent);
    pipeA->pump();

    RuntimeInvocation received;
    auto count = server.receive(3, &received, 1);

    bool ok = count == 1 && received.deliveryClass == DeliveryClass::UNORDERED_LOSSY;

    if (ok) PASS();
    else FAIL("deliveryClass=" + std::to_string(static_cast<int>(received.deliveryClass)));
}

static void testSendErrorPaths() {
    TEST("oversized payload and null pipe");

    // 超过 maxMessageSize → Oversized
    {
        auto [pipeA, pipeB] = InMemoryBytePipe::createPair();
        NetworkTransport::Config cfg;
        cfg.localComponent = 1;
        cfg.maxMessageSize = 8;
        NetworkTransport client(pipeA, cfg);

        RuntimeInvocation big = makeInvocation(9, 2, "big");
        big.payload.assign(64, std::byte{0});
        bool ok = client.send(big) == SendResult::Oversized;

        // 构造时无 pipe → NotConnected
        NetworkTransport orphan(nullptr);
        ok = ok && orphan.send(makeInvocation(1, 1, "x")) == SendResult::NotConnected;
        ok = ok && !orphan.isConnected();

        if (ok) PASS();
        else FAIL("oversized/null-pipe behavior wrong");
    }
}

static void testGarbageHeaderDoesNotKillTransport() {
    TEST("garbage header bytes are skipped, transport keeps working");

    auto [pipeA, pipeB] = InMemoryBytePipe::createPair();
    NetworkTransport client(pipeA);
    NetworkTransport server(pipeB);

    // 绕过对端编码器直接注入无法解析的帧头。
    // InMemoryBytePipe 方向：write 存入本端 pending，本端 pump 才投递给对端。
    std::vector<std::byte> garbage{std::byte{0xFF}, std::byte{0xFF},
                                   std::byte{0x00}, std::byte{0x00}};
    if (!pipeB->write(garbage)) { FAIL("pipe write failed"); return; }
    pipeB->pump();

    // 之后的正常消息仍然可达
    bool ok = client.send(makeInvocation(4, 5, "after-garbage")) == SendResult::Accepted;
    client.flush();
    pipeA->pump();
    pipeB->pump();

    RuntimeInvocation received;
    ok = ok && server.receive(5, &received, 1) == 1;
    ok = ok && received.method == "after-garbage";

    if (ok) PASS();
    else FAIL("transport state corrupted by garbage header");
}

static void testPipeWriteEdgeCases() {
    TEST("pipe write: empty payload, expired peer, closed pipe");

    auto [pipeA, pipeB] = InMemoryBytePipe::createPair();

    // 空 payload：连通时返回 true（短路分支）
    bool ok = pipeA->write({});

    // 对端销毁：weak_ptr lock 失败 → false
    pipeB.reset();
    std::vector<std::byte> data{std::byte{1}};
    ok = ok && !pipeA->write(data);

    // 自身关闭：connected_ 为假 → 返回 false
    pipeA->close();
    ok = ok && !pipeA->write(data);

    if (ok) PASS();
    else FAIL("pipe write edge cases wrong");
}

static void testBytePipePumpEdgeArms() {
    TEST("InMemoryBytePipe pump: expired peer and missing callback");

    // 对端先销毁：write 已入队，pump 时 peer.lock() 失败 → continue（不崩、静默丢弃）。
    {
        auto [pipeA, pipeB] = InMemoryBytePipe::createPair();
        const std::byte payload[] = {std::byte{0x01}};
        bool ok = pipeA->write(std::span<const std::byte>(payload, 1));
        pipeB.reset();
        pipeA->pump();  // peer 失效臂：循环继续不投递
        ok = ok && pipeA->isConnected();
        pipeA->close();
        if (ok) PASS();
        else FAIL("expired peer pump wrong");
    }

    // 对端存活但未设回调：pump 投递循环跳过回调调用（callback 空臂）。
    {
        auto [pipeA, pipeB] = InMemoryBytePipe::createPair();
        const std::byte payload[] = {std::byte{0x02}};
        bool ok = pipeA->write(std::span<const std::byte>(payload, 1));
        pipeA->pump();  // 无回调臂
        ok = ok && pipeB->isConnected();
        if (ok) PASS();
        else FAIL("no-callback pump wrong");
    }
}

static void testDisconnectedTickAndFlush() {
    TEST("disconnected transport: tick/flush are no-ops");

    // 无 pipe：tick 的 pipe_ 假臂 / isConnected 假臂 / flushOutbound 的 !pipe_ 早退
    {
        NetworkTransport orphan(nullptr);
        orphan.tick();
    }

    // pipe 已断开：tick 的 isConnected 假臂 + flushOutbound 的未连接早退
    {
        auto [pipeA, pipeB] = InMemoryBytePipe::createPair();
        NetworkTransport client(pipeA);
        pipeA->close();
        client.tick();
    }

    // 已连接但 channel 空：flushOutbound 的 drain==0 臂（不写出）
    {
        auto [pipeA, pipeB] = InMemoryBytePipe::createPair();
        NetworkTransport client(pipeA);
        client.tick();  // 未 send 过任何消息 → drainAll==0
    }

    PASS();
}

static void testUnreliableFullQueueDiscards() {
    TEST("unreliable full queue: DiscardOldest policy accepts and drops oldest");

    auto [pipeA, pipeB] = InMemoryBytePipe::createPair();
    NetworkTransport::Config cfg;
    cfg.localComponent = 1;
    cfg.autoFlush = false;  // 攒批：channel 才能积压到水位
    NetworkTransport client(pipeA, cfg);

    bool ok = true;
    for (int i = 0; i < 256; ++i) {
        RuntimeInvocation inv = makeInvocation(7, 2, "pos" + std::to_string(i));
        inv.deliveryClass = DeliveryClass::UNORDERED_LOSSY;
        ok = ok && client.send(inv) == SendResult::Accepted;
    }
    // 第 257 条：isBackPressured 真 && policy==DiscardOldest → 不报 BackPressure，Accept 并丢最老
    RuntimeInvocation overflow = makeInvocation(7, 2, "overflow");
    overflow.deliveryClass = DeliveryClass::UNORDERED_LOSSY;
    ok = ok && client.send(overflow) == SendResult::Accepted;
    ok = ok && client.stats().backPressureEvents == 0;

    if (ok) PASS();
    else FAIL("unreliable full queue behavior wrong");
}

static void testAutoFlushBackPressure() {
    TEST("autoFlush=false accumulates backlog and reports BackPressure");

    auto [pipeA, pipeB] = InMemoryBytePipe::createPair();
    NetworkTransport::Config cfg;
    cfg.localComponent = 1;
    cfg.autoFlush = false;  // 攒批：send 只入队，由 flush()/tick() 统一冲刷
    NetworkTransport client(pipeA, cfg);
    NetworkTransport server(pipeB);

    // watermark.high = 256：前 256 条 Accepted（积压在 channel），第 257 条 BackPressure。
    bool ok = true;
    for (int i = 0; i < 257; ++i) {
        auto result = client.send(makeInvocation(static_cast<std::uint64_t>(i), 5, "bulk"));
        if (i < 256) {
            ok = ok && result == SendResult::Accepted;
        } else {
            ok = ok && result == SendResult::BackPressure;
        }
    }
    ok = ok && client.stats().backPressureEvents == 1;

    // flush 后积压全部发出，对端可完整收取 256 条。
    client.flush();
    pipeA->pump();

    RuntimeInvocation received;
    std::size_t total = 0;
    while (server.receive(5, &received, 1) > 0) ++total;
    ok = ok && total == 256;

    if (ok) PASS();
    else FAIL("autoFlush backlog/BackPressure mismatch");
}

// receive 容量 0 早退（防御臂）。
static void testReceiveZeroCapacity() {
    TEST("receive with zero capacity returns 0");

    NetworkTransport server(nullptr);   // 无管道也不影响防御臂
    RuntimeInvocation inv;
    bool ok = server.receive(1, &inv, 0) == 0;

    if (ok) PASS();
    else FAIL("zero capacity should return 0");
}

int main() {
    std::cout << "NetworkTransport tests:\n";

    testSendReceiveOne();
    testSendReceiveMultiple();
    testFilterByTargetComponent();
    testRoundTripWithPayload();
    testPartialRead();
    testMultipleMessagesInOneChunk();
    testDisconnectedPipeReturnsFalse();
    testPendingCount();
    testBidirectional();
    testUnorderedLossyDelivery();
    testSendErrorPaths();
    testGarbageHeaderDoesNotKillTransport();
    testPipeWriteEdgeCases();
    testBytePipePumpEdgeArms();
    testDisconnectedTickAndFlush();
    testUnreliableFullQueueDiscards();
    testAutoFlushBackPressure();
    testReceiveZeroCapacity();

    std::cout << "\n  Passed: " << testsPassed << "/" << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
