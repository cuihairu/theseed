#pragma once

#include "theseed/runtime/RuntimeTransport.h"

#include <cstddef>
#include <span>
#include <vector>

namespace theseed::runtime {

class InvocationCodec final {
public:
    static std::vector<std::byte> encode(const RuntimeInvocation& invocation);
    static RuntimeInvocation decode(std::span<const std::byte> data);

    InvocationCodec() = delete;
};

}  // namespace theseed::runtime
