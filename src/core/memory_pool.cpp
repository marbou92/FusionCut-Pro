#include "memory_pool.h"

#include <cstdlib>

namespace fc {

namespace {

constexpr std::size_t kAlignment = 64; // SIMD-friendly block alignment

std::size_t alignUp(std::size_t value, std::size_t alignment) {
    return ((value + alignment - 1) / alignment) * alignment;
}

} // namespace

MemoryPool::MemoryPool(std::size_t blockSize, std::size_t blockCount)
    : blockSize_(blockSize), stride_(alignUp(blockSize, kAlignment)), blockCount_(blockCount) {
    if (blockSize_ == 0 || blockCount_ == 0) {
        blockCount_ = 0;
        blockSize_ = blockSize; // keep requested size, but pool stays inert
        stride_ = 0;
        return;
    }

    // One raw allocation; alignment slack is carved off the front.
    allocBytes_ = stride_ * blockCount_ + kAlignment;
    base_ = static_cast<unsigned char *>(::operator new(allocBytes_));
    const std::uintptr_t raw = reinterpret_cast<std::uintptr_t>(base_);
    first_ = reinterpret_cast<unsigned char *>(alignUp(raw, kAlignment));

    nextFree_.resize(blockCount_);
    used_.assign(blockCount_, 0);
    for (std::size_t i = 0; i < blockCount_; ++i) {
        nextFree_[i] = (i + 1 < blockCount_) ? static_cast<int64_t>(i + 1) : -1;
    }
    head_ = 0;
}

MemoryPool::~MemoryPool() {
    ::operator delete(base_);
}

void *MemoryPool::acquire() {
    if (head_ < 0) {
        return nullptr;
    }
    const std::size_t index = static_cast<std::size_t>(head_);
    head_ = nextFree_[index];
    used_[index] = 1;
    ++inUse_;
    return blockAt(index);
}

bool MemoryPool::release(void *block) {
    if (!owns(block)) {
        return false;
    }
    const std::size_t index = indexOf(block);
    if (!used_[index]) {
        return false; // already released (double free attempt)
    }
    used_[index] = 0;
    nextFree_[index] = head_;
    head_ = static_cast<int64_t>(index);
    --inUse_;
    return true;
}

bool MemoryPool::owns(const void *block) const noexcept {
    if (block == nullptr || first_ == nullptr || blockCount_ == 0) {
        return false;
    }
    const unsigned char *p = static_cast<const unsigned char *>(block);
    if (p < first_) {
        return false;
    }
    const std::size_t offset = static_cast<std::size_t>(p - first_);
    if (offset >= stride_ * blockCount_) {
        return false;
    }
    return (offset % stride_) == 0;
}

void *MemoryPool::blockAt(std::size_t index) const noexcept {
    return first_ + (index * stride_);
}

std::size_t MemoryPool::indexOf(const void *block) const noexcept {
    const unsigned char *p = static_cast<const unsigned char *>(block);
    return static_cast<std::size_t>(p - first_) / stride_;
}

} // namespace fc
