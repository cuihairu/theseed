#include "theseed/runtime/IORuntime.h"

#include <utility>

namespace theseed::runtime {

InMemoryIORuntime::InMemoryIORuntime() = default;

void InMemoryIORuntime::completePendingRequests() {
    for (const auto& [tokenValue, pending] : pendingRequests_) {
        completions_.push_back(IoCompletion{
            .token = IoToken{tokenValue},
            .status = IoStatus::Ok,
            .bytesTransferred = 0,
            .osError = 0,
            .userData = pending.request.userData,
        });
    }

    pendingRequests_.clear();
}

void InMemoryIORuntime::runOnce(Duration maxWait) {
    std::unique_lock lock(mutex_);
    if (pendingRequests_.empty() && completions_.empty() && !wakeRequested_ &&
        maxWait > Duration::zero()) {
        wakeupSignal_.wait_for(lock, maxWait, [&] {
            return !pendingRequests_.empty() || !completions_.empty() || wakeRequested_;
        });
    }

    if (pendingRequests_.empty() && completions_.empty() && !wakeRequested_) {
        return;
    }

    wakeRequested_ = false;
    completePendingRequests();
}

void InMemoryIORuntime::wakeup() {
    {
        std::lock_guard lock(mutex_);
        wakeRequested_ = true;
    }

    wakeupSignal_.notify_all();
}

IoToken InMemoryIORuntime::submit(const IoRequest& request) {
    std::lock_guard lock(mutex_);

    const IoToken token{nextToken_++};
    pendingRequests_.emplace(token.value, PendingRequest{request});
    wakeupSignal_.notify_all();
    return token;
}

bool InMemoryIORuntime::cancel(IoToken token) {
    std::lock_guard lock(mutex_);
    const auto iter = pendingRequests_.find(token.value);
    if (iter == pendingRequests_.end()) {
        return false;
    }

    completions_.push_back(IoCompletion{
        .token = token,
        .status = IoStatus::Cancelled,
        .bytesTransferred = 0,
        .osError = 0,
        .userData = iter->second.request.userData,
    });
    pendingRequests_.erase(iter);
    return true;
}

std::size_t InMemoryIORuntime::drainCompletions(IoCompletion* out, std::size_t capacity) {
    if (out == nullptr || capacity == 0) {
        return 0;
    }

    std::lock_guard lock(mutex_);
    std::size_t count = 0;
    while (count < capacity && !completions_.empty()) {
        out[count] = completions_.front();
        completions_.pop_front();
        ++count;
    }

    return count;
}

std::size_t InMemoryIORuntime::pendingRequestCount() const {
    std::lock_guard lock(mutex_);
    return pendingRequests_.size();
}

std::size_t InMemoryIORuntime::completionCount() const {
    std::lock_guard lock(mutex_);
    return completions_.size();
}

}  // namespace theseed::runtime
