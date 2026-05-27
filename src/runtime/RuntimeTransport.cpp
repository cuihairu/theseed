#include "theseed/runtime/RuntimeTransport.h"

#include <utility>

namespace theseed::runtime {

SendResult InMemoryRuntimeTransport::send(RuntimeInvocation invocation) {
    std::lock_guard lock(mutex_);
    ++stats_.messagesSent;
    invocations_.push_back(std::move(invocation));
    return SendResult::Accepted;
}

std::size_t InMemoryRuntimeTransport::receive(ComponentId targetComponent,
                                              RuntimeInvocation* out,
                                              std::size_t capacity) {
    if (targetComponent == 0 || out == nullptr || capacity == 0) {
        return 0;
    }

    std::lock_guard lock(mutex_);
    std::deque<RuntimeInvocation> remaining;
    std::size_t count = 0;
    while (!invocations_.empty()) {
        auto invocation = std::move(invocations_.front());
        invocations_.pop_front();

        if (count < capacity && invocation.targetComponent == targetComponent) {
            out[count] = std::move(invocation);
            ++count;
            continue;
        }

        remaining.push_back(std::move(invocation));
    }

    invocations_.swap(remaining);
    stats_.messagesReceived += count;
    return count;
}

std::size_t InMemoryRuntimeTransport::drain(RuntimeInvocation* out, std::size_t capacity) {
    if (out == nullptr || capacity == 0) {
        return 0;
    }

    std::lock_guard lock(mutex_);
    std::size_t count = 0;
    while (count < capacity && !invocations_.empty()) {
        out[count] = std::move(invocations_.front());
        invocations_.pop_front();
        ++count;
    }

    return count;
}

std::size_t InMemoryRuntimeTransport::pendingCount() const {
    std::lock_guard lock(mutex_);
    return invocations_.size();
}

void InMemoryRuntimeTransport::flush() {
    // In-memory transport has no outbound buffering.
}

TransportStats InMemoryRuntimeTransport::stats() const {
    std::lock_guard lock(mutex_);
    auto s = stats_;
    s.inboundQueueDepth = invocations_.size();
    return s;
}

}  // namespace theseed::runtime
