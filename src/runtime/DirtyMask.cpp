#include "theseed/runtime/DirtyMask.h"

#include <bit>

namespace theseed::runtime {

DirtyMask::DirtyMask(std::size_t propertyCount) {
    resize(propertyCount);
}

std::size_t DirtyMask::wordCountFor(std::size_t propertyCount) {
    return propertyCount == 0 ? 0 : ((propertyCount - 1) / bitsPerWord) + 1;
}

void DirtyMask::resize(std::size_t propertyCount) {
    words_.assign(wordCountFor(propertyCount), 0);
}

void DirtyMask::clear() {
    for (auto& word : words_) {
        word = 0;
    }
}

void DirtyMask::mark(PropertyId id) {
    const auto wordIndex = static_cast<std::size_t>(id) / bitsPerWord;
    const auto bitIndex = static_cast<std::size_t>(id) % bitsPerWord;
    if (wordIndex >= words_.size()) {
        words_.resize(wordIndex + 1, 0);
    }

    words_[wordIndex] |= (std::uint64_t{1} << bitIndex);
}

bool DirtyMask::isDirty(PropertyId id) const {
    const auto wordIndex = static_cast<std::size_t>(id) / bitsPerWord;
    const auto bitIndex = static_cast<std::size_t>(id) % bitsPerWord;
    if (wordIndex >= words_.size()) {
        return false;
    }

    return (words_[wordIndex] & (std::uint64_t{1} << bitIndex)) != 0;
}

bool DirtyMask::any() const {
    for (const auto word : words_) {
        if (word != 0) {
            return true;
        }
    }

    return false;
}

}  // namespace theseed::runtime
