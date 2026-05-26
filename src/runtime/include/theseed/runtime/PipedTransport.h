#pragma once

#include "theseed/runtime/RuntimeTransport.h"

#include <cstddef>
#include <deque>
#include <mutex>
#include <span>

namespace theseed::runtime {

class PipedTransport final : public IRuntimeTransport {
public:
    explicit PipedTransport(ComponentId localComponent);

    PipedTransport(const PipedTransport&) = delete;
    PipedTransport& operator=(const PipedTransport&) = delete;

    bool send(RuntimeInvocation invocation) override;
    std::size_t receive(ComponentId targetComponent,
                        RuntimeInvocation* out,
                        std::size_t capacity) override;
    std::size_t pendingCount() const override;

    void connect(PipedTransport& peer);
    void flush();

private:
    ComponentId localComponent_;
    PipedTransport* peer_ = nullptr;
    mutable std::mutex mutex_;
    std::deque<RuntimeInvocation> inbox_;
};

}  // namespace theseed::runtime
