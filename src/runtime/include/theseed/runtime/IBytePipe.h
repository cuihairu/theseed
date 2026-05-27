#pragma once

#include <cstddef>
#include <functional>
#include <span>

namespace theseed::runtime {

class IBytePipe {
public:
    virtual ~IBytePipe() = default;

    virtual bool write(std::span<const std::byte> data) = 0;
    virtual void pump() = 0;
    virtual void setOnReceived(std::function<void(std::span<const std::byte>)> callback) = 0;
    virtual void close() = 0;
    virtual bool isConnected() const = 0;
};

}  // namespace theseed::runtime
