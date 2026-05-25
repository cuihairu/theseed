#pragma once

#include "theseed/runtime/RuntimeTypes.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>

namespace theseed::runtime {

enum class IoOp : std::uint8_t {
    Accept = 0,
    Connect,
    Read,
    Write,
    RecvFrom,
    SendTo,
};

struct IoHandle {
    std::uint64_t value = 0;
    std::uint32_t generation = 0;
};

struct IoBuffer {
    void* data = nullptr;
    std::uint32_t size = 0;
};

struct IoRequest {
    IoOp op = IoOp::Read;
    IoHandle handle{};
    IoBuffer buffer{};
    void* userData = nullptr;
};

struct IoToken {
    std::uint64_t value = 0;
};

enum class IoStatus : std::uint8_t {
    Ok = 0,
    Cancelled,
    Timeout,
    Closed,
    Error,
};

struct IoCompletion {
    IoToken token{};
    IoStatus status = IoStatus::Ok;
    std::uint32_t bytesTransferred = 0;
    int osError = 0;
    void* userData = nullptr;
};

class IIORuntime {
public:
    virtual ~IIORuntime() = default;

    virtual void runOnce(Duration maxWait) = 0;
    virtual void wakeup() = 0;

    virtual IoToken submit(const IoRequest& request) = 0;
    virtual bool cancel(IoToken token) = 0;

    virtual std::size_t drainCompletions(IoCompletion* out, std::size_t capacity) = 0;
};

class InMemoryIORuntime final : public IIORuntime {
public:
    InMemoryIORuntime();

    InMemoryIORuntime(const InMemoryIORuntime&) = delete;
    InMemoryIORuntime& operator=(const InMemoryIORuntime&) = delete;

    void runOnce(Duration maxWait) override;
    void wakeup() override;

    IoToken submit(const IoRequest& request) override;
    bool cancel(IoToken token) override;

    std::size_t drainCompletions(IoCompletion* out, std::size_t capacity) override;

    std::size_t pendingRequestCount() const;
    std::size_t completionCount() const;

private:
    struct PendingRequest {
        IoRequest request;
    };

    void completePendingRequests();

    mutable std::mutex mutex_;
    std::condition_variable wakeupSignal_;
    std::map<std::uint64_t, PendingRequest> pendingRequests_;
    std::deque<IoCompletion> completions_;
    std::uint64_t nextToken_ = 1;
    bool wakeRequested_ = false;
};

}  // namespace theseed::runtime
