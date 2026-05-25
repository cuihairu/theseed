#include "theseed/runtime/EntityCall.h"
#include "theseed/runtime/RuntimeTransport.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>

using theseed::runtime::DeliveryClass;
using theseed::runtime::EntityCall;
using theseed::runtime::InMemoryRuntimeTransport;
using theseed::runtime::RuntimeInvocation;

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

    if (!call.call(transport, "move", payload)) {
        return fail("call_send");
    }

    if (transport.pendingCount() != 1) {
        return fail("pending_count");
    }

    std::array<RuntimeInvocation, 4> drained{};
    const auto drainedCount = transport.drain(drained.data(), drained.size());
    if (drainedCount != 1) {
        return fail("drain_count");
    }

    if (drained[0].entityId != 42 || drained[0].method != "move") {
        return fail("drain_content");
    }

    call.updateTarget(9);
    call.setDeliveryClass(DeliveryClass::UNORDERED_LOSSY);
    if (!call.call(transport, "ping", {})) {
        return fail("call_after_update");
    }
    EntityCall otherCall(99, 7, "Monster");
    if (!otherCall.call(transport, "attack", {})) {
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
    if (call.call(transport, "fail", {})) {
        return fail("invalid_call");
    }

    return EXIT_SUCCESS;
}
