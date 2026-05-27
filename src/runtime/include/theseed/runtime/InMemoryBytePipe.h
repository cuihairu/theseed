#pragma once

#include "theseed/runtime/IBytePipe.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace theseed::runtime {

class InMemoryBytePipe final : public IBytePipe {
public:
    static std::pair<std::shared_ptr<InMemoryBytePipe>,
                     std::shared_ptr<InMemoryBytePipe>> createPair();

    bool write(std::span<const std::byte> data) override;
    void pump() override;
    void setOnReceived(std::function<void(std::span<const std::byte>)> callback) override;
    void close() override;
    bool isConnected() const override;

private:
    InMemoryBytePipe() = default;

    void deliverPending();

    std::weak_ptr<InMemoryBytePipe> peer_;
    std::function<void(std::span<const std::byte>)> onReceived_;
    std::vector<std::vector<std::byte>> pending_;
    mutable std::mutex mutex_;
    bool connected_ = true;
};

}  // namespace theseed::runtime
