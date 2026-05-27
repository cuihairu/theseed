#include "theseed/runtime/InMemoryBytePipe.h"

namespace theseed::runtime {

std::pair<std::shared_ptr<InMemoryBytePipe>,
          std::shared_ptr<InMemoryBytePipe>> InMemoryBytePipe::createPair() {
    auto a = std::shared_ptr<InMemoryBytePipe>(new InMemoryBytePipe());
    auto b = std::shared_ptr<InMemoryBytePipe>(new InMemoryBytePipe());
    a->peer_ = b;
    b->peer_ = a;
    return {a, b};
}

bool InMemoryBytePipe::write(std::span<const std::byte> data) {
    std::lock_guard lock(mutex_);
    if (!connected_ || data.empty()) {
        return connected_;
    }

    auto peer = peer_.lock();
    if (!peer) {
        return false;
    }

    std::vector<std::byte> copy(data.begin(), data.end());
    pending_.push_back(std::move(copy));
    return true;
}

void InMemoryBytePipe::setOnReceived(std::function<void(std::span<const std::byte>)> callback) {
    std::lock_guard lock(mutex_);
    onReceived_ = std::move(callback);
}

void InMemoryBytePipe::close() {
    std::lock_guard lock(mutex_);
    connected_ = false;
    onReceived_ = nullptr;
    pending_.clear();
}

bool InMemoryBytePipe::isConnected() const {
    std::lock_guard lock(mutex_);
    return connected_;
}

void InMemoryBytePipe::pump() {
    std::vector<std::vector<std::byte>> toDeliver;
    {
        std::lock_guard lock(mutex_);
        toDeliver = std::move(pending_);
        pending_.clear();
    }

    for (auto& chunk : toDeliver) {
        auto peer = peer_.lock();
        if (!peer) continue;

        std::function<void(std::span<const std::byte>)> callback;
        {
            std::lock_guard peerLock(peer->mutex_);
            callback = peer->onReceived_;
        }

        if (callback) {
            callback(std::span<const std::byte>(chunk.data(), chunk.size()));
        }
    }
}

}  // namespace theseed::runtime
