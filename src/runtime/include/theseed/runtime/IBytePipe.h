#pragma once

#include <cstddef>
#include <functional>
#include <span>

namespace theseed::runtime {

class IBytePipe {
public:
    virtual ~IBytePipe() = default;  // LCOV_EXCL_LINE C++ ABI：trivial 虚析构是空体，gcc 不为其生成计数指令，D0/D1/D2 三符号变体恒 0（结构不可测）

    virtual bool write(std::span<const std::byte> data) = 0;
    virtual void pump() = 0;
    virtual void setOnReceived(std::function<void(std::span<const std::byte>)> callback) = 0;
    virtual void close() = 0;
    virtual bool isConnected() const = 0;
};

}  // namespace theseed::runtime
