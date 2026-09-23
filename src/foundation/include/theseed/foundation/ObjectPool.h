#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace theseed::foundation::detail {

// 跨平台对齐内存分配：Windows CRT 与 C++17 std::aligned_alloc 双路径。
inline void* alignedAlloc(std::size_t size, std::size_t alignment) {
#if defined(_WIN32)
    return _aligned_malloc(size, alignment);
#else
    if (alignment < alignof(std::max_align_t)) {
        alignment = alignof(std::max_align_t);
    }
    const std::size_t padded = (size + alignment - 1) / alignment * alignment;
    return std::aligned_alloc(alignment, padded);
#endif
}

inline void alignedFree(void* ptr) {
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

}  // namespace theseed::foundation::detail

namespace theseed::foundation {

template <typename T>
class ObjectPool final {
public:
    static constexpr std::size_t kDefaultBlockSize = 64;

    explicit ObjectPool(std::size_t blockSize = kDefaultBlockSize,
                        std::function<void(T&)> resetFn = nullptr);
    ~ObjectPool();

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    template <typename... Args>
    T* acquire(Args&&... args);

    void release(T* ptr);

    std::size_t activeCount() const;
    std::size_t totalCount() const;
    std::size_t highWatermark() const;
    std::size_t blockCount() const;

    void shrink();

private:
    struct Block {
        void* memory;
        std::size_t count;
    };

    void addBlock();

    std::size_t blockSize_;
    std::function<void(T&)> resetFn_;
    std::vector<Block> blocks_;
    std::vector<T*> freeList_;
    std::size_t activeCount_ = 0;
    std::size_t highWatermark_ = 0;
};

template <typename T>
class PooledObject final {
public:
    PooledObject() = default;
    PooledObject(ObjectPool<T>& pool, T* ptr) : pool_(&pool), ptr_(ptr) {}

    ~PooledObject() {
        if (ptr_ && pool_) pool_->release(ptr_);
    }

    PooledObject(PooledObject&& other) noexcept
        : pool_(other.pool_), ptr_(other.ptr_) {
        other.pool_ = nullptr;
        other.ptr_ = nullptr;
    }

    PooledObject& operator=(PooledObject&& other) noexcept {
        if (this != &other) {
            if (ptr_ && pool_) pool_->release(ptr_);
            pool_ = other.pool_;
            ptr_ = other.ptr_;
            other.pool_ = nullptr;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    PooledObject(const PooledObject&) = delete;
    PooledObject& operator=(const PooledObject&) = delete;

    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    T& operator*() const { return *ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

private:
    ObjectPool<T>* pool_ = nullptr;
    T* ptr_ = nullptr;
};

// Implementation

template <typename T>
ObjectPool<T>::ObjectPool(std::size_t blockSize, std::function<void(T&)> resetFn)
    : blockSize_(blockSize), resetFn_(std::move(resetFn)) {}

template <typename T>
ObjectPool<T>::~ObjectPool() {
    for (auto& block : blocks_) {
        detail::alignedFree(block.memory);
    }
}

template <typename T>
void ObjectPool<T>::addBlock() {
    const auto alignment = alignof(T);
    const auto size = sizeof(T) * blockSize_;

    auto* memory = detail::alignedAlloc(size, alignment);
    if (!memory) throw std::bad_alloc();

    Block block{memory, blockSize_};
    blocks_.push_back(block);

    auto* objects = static_cast<T*>(memory);
    for (std::size_t i = 0; i < blockSize_; ++i) {
        freeList_.push_back(&objects[i]);
    }
}

template <typename T>
template <typename... Args>
T* ObjectPool<T>::acquire(Args&&... args) {
    if (freeList_.empty()) {
        addBlock();
    }

    auto* ptr = freeList_.back();
    freeList_.pop_back();

    new (ptr) T(std::forward<Args>(args)...);

    ++activeCount_;
    if (activeCount_ > highWatermark_) {
        highWatermark_ = activeCount_;
    }

    return ptr;
}

template <typename T>
void ObjectPool<T>::release(T* ptr) {
    if (!ptr) return;

    if (resetFn_) {
        resetFn_(*ptr);
    }
    ptr->~T();

    freeList_.push_back(ptr);
    --activeCount_;
}

template <typename T>
std::size_t ObjectPool<T>::activeCount() const { return activeCount_; }

template <typename T>
std::size_t ObjectPool<T>::totalCount() const { return blocks_.size() * blockSize_; }

template <typename T>
std::size_t ObjectPool<T>::highWatermark() const { return highWatermark_; }

template <typename T>
std::size_t ObjectPool<T>::blockCount() const { return blocks_.size(); }

template <typename T>
void ObjectPool<T>::shrink() {
    // Only release blocks that are completely free
    // This is a no-op for simplicity; full implementation would scan blocks
}

}  // namespace theseed::foundation
