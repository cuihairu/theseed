#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace theseed::runtime {

using PropertyId = std::uint32_t;

class DirtyMask final {
public:
    DirtyMask() = default;
    explicit DirtyMask(std::size_t propertyCount);

    void resize(std::size_t propertyCount);
    void clear();

    void mark(PropertyId id);
    bool isDirty(PropertyId id) const;
    bool any() const;

    template <typename Func>
    void forEachDirty(Func&& fn) const {
        for (std::size_t wordIndex = 0; wordIndex < words_.size(); ++wordIndex) {
            auto word = words_[wordIndex];
            while (word != 0) {
                const auto bit = static_cast<std::uint32_t>(std::countr_zero(word));
                fn(static_cast<PropertyId>(wordIndex * bitsPerWord + bit));
                word &= ~(std::uint64_t{1} << bit);
            }
        }
    }

private:
    static constexpr std::size_t bitsPerWord = 64;

    static std::size_t wordCountFor(std::size_t propertyCount);

    std::vector<std::uint64_t> words_;
};

}  // namespace theseed::runtime
