#include "theseed/runtime/EntityCall.h"
#include "theseed/runtime/PipedTransport.h"
#include "theseed/runtime/RuntimeTransport.h"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>

using theseed::runtime::DeliveryClass;
using theseed::runtime::EntityCall;
using theseed::runtime::InMemoryRuntimeTransport;
using theseed::runtime::PipedTransport;
using theseed::runtime::RuntimeInvocation;
using theseed::runtime::SendResult;

namespace {

int fail(const char* stage) {
    std::cerr << "runtime_transport_test_failed_at=" << stage << '\n';
    return EXIT_FAILURE;
}

}  // namespace

int main() {
    InMemoryRuntimeTransport transport;
    EntityCall call(42, 7, "Avatar");

    const std::array<std::byte, 3> payload{
        std::byte{0x01},
        std::byte{0x02},
        std::byte{0x03},
    };

    auto invocation = call.buildInvocation("move", payload);
    if (invocation.entityId != 42 || invocation.targetComponent != 7) {
        return fail("build_identity");
    }
    if (invocation.entityType != "Avatar" || invocation.method != "move") {
        return fail("build_content");
    }
    if (invocation.deliveryClass != DeliveryClass::ORDERED_RELIABLE) {
        return fail("build_delivery");
    }
    if (invocation.payload.size() != payload.size()) {
        return fail("build_payload_size");
    }

    if (call.call(transport, "move", payload) != SendResult::Accepted) {
        return fail("call_send");
    }

    if (transport.pendingCount() != 1) {
        return fail("pending_count");
    }

    std::array<RuntimeInvocation, 4> drained{};

    // receive/drain 的非法参数直接返回 0，且不得扰动队列
    if (transport.receive(0, drained.data(), drained.size()) != 0) {
        return fail("receive_zero_component");
    }
    if (transport.receive(7, nullptr, drained.size()) != 0) {
        return fail("receive_null_out");
    }
    if (transport.receive(7, drained.data(), 0) != 0) {
        return fail("receive_zero_capacity");
    }
    if (transport.drain(nullptr, drained.size()) != 0) {
        return fail("drain_null_out");
    }
    if (transport.drain(drained.data(), 0) != 0) {
        return fail("drain_zero_capacity");
    }
    if (transport.pendingCount() != 1) {
        return fail("param_checks_disturbed_queue");
    }

    const auto drainedCount = transport.drain(drained.data(), drained.size());
    if (drainedCount != 1) {
        return fail("drain_count");
    }

    if (drained[0].entityId != 42 || drained[0].method != "move") {
        return fail("drain_content");
    }

    call.updateTarget(9);
    call.setDeliveryClass(DeliveryClass::UNORDERED_LOSSY);
    if (call.call(transport, "ping", {}) != SendResult::Accepted) {
        return fail("call_after_update");
    }
    EntityCall otherCall(99, 7, "Monster");
    if (otherCall.call(transport, "attack", {}) != SendResult::Accepted) {
        return fail("call_second_target");
    }

    const auto targetedCount = transport.receive(9, drained.data(), drained.size());
    if (targetedCount != 1 || drained[0].targetComponent != 9 ||
        drained[0].deliveryClass != DeliveryClass::UNORDERED_LOSSY) {
        return fail("receive_targeted");
    }
    if (transport.pendingCount() != 1) {
        return fail("receive_targeted_pending");
    }

    const auto secondDrainCount = transport.drain(drained.data(), drained.size());
    if (secondDrainCount != 1 || drained[0].targetComponent != 7 ||
        drained[0].entityId != 99 || drained[0].method != "attack") {
        return fail("drain_remaining");
    }

    call.invalidate();
    if (call.call(transport, "fail", {}) == SendResult::Accepted) {
        return fail("invalid_call");
    }

    // tick 契约：内存实现的 tick 是空操作——不投递、不扰动队列。
    transport.tick();
    if (transport.pendingCount() != 0) {
        return fail("tick_noop_pending");
    }

    // PipedTransport 不覆盖 tick，走基类默认空实现（同样不得抛出或出错）。
    PipedTransport piped(1);
    piped.tick();

    // PipedTransport receive：容量打满即退出；目标不匹配的消息保留在箱内。
    {
        PipedTransport piped2(1);
        PipedTransport piped2Peer(9);   // send 需要对端：connect 后才 Accepted
        piped2.connect(piped2Peer);
        RuntimeInvocation a;
        a.sourceComponent = 1; a.targetComponent = 2; a.method = "a";
        RuntimeInvocation b;
        b.sourceComponent = 1; b.targetComponent = 3; b.method = "b";
        RuntimeInvocation c;
        c.sourceComponent = 1; c.targetComponent = 2; c.method = "c";
        if (piped2.send(a) != SendResult::Accepted || piped2.send(b) != SendResult::Accepted ||
            piped2.send(c) != SendResult::Accepted) {
            return fail("piped_send");
        }

        std::array<RuntimeInvocation, 2> out{};
        // send 进的是对端收件箱：在 piped2Peer 上 receive。
        // 容量 1：取走第一条 target=2 的，target=3 的跳过保留（++it 分支），count 达容量即 break。
        if (piped2Peer.receive(2, out.data(), 1) != 1 || out[0].method != "a") {
            return fail("piped_receive_cap");
        }
        // 容量 2：剩余 target=2 的 c 可取，b 仍在箱内。
        if (piped2Peer.receive(2, out.data(), 2) != 1 || out[0].method != "c") {
            return fail("piped_receive_skip");
        }
        if (piped2Peer.receive(3, out.data(), 2) != 1 || out[0].method != "b") {
            return fail("piped_receive_b");
        }
    }

    // InMemoryRuntimeTransport drain：容量打满退出（循环假臂），余量保留。
    {
        InMemoryRuntimeTransport t2;
        RuntimeInvocation a;
        a.sourceComponent = 1; a.targetComponent = 1; a.method = "a";
        RuntimeInvocation b;
        b.sourceComponent = 1; b.targetComponent = 1; b.method = "b";
        t2.send(a);
        t2.send(b);
        std::array<RuntimeInvocation, 2> out{};
        if (t2.drain(out.data(), 1) != 1 || out[0].method != "a") {
            return fail("drain_cap");
        }
        if (t2.drain(out.data(), 2) != 1 || out[0].method != "b") {
            return fail("drain_rest");
        }
    }

    return EXIT_SUCCESS;
}
