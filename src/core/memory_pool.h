#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fc {

// Fixed-capacity pool of equally sized, 64-byte aligned memory blocks.
//
// Purpose: deterministic video-frame allocation under a strict RAM budget
// (Module 3: "custom memory pool allocator for video frames").
//  - Never grows: acquire() returns nullptr when exhausted, so there are
//    no surprise allocations that could blow the 1GB target.
//  - No per-acquire malloc: all blocks are carved out of one allocation.
//  - release() validates ownership and state, making double-release and
//    foreign-pointer bugs detectable instead of corrupt.
class MemoryPool {
public:
    MemoryPool() = default;

    // blockSize: usable bytes per block. blockCount: total number of
    // blocks. Blocks are stride-aligned so every block start is 64-byte
    // aligned (SIMD friendly).
    MemoryPool(std::size_t blockSize, std::size_t blockCount);
    ~MemoryPool();

    MemoryPool(const MemoryPool &) = delete;
    MemoryPool &operator=(const MemoryPool &) = delete;

    // Returns one aligned block, or nullptr when the pool is exhausted.
    void *acquire();

    // Frees a block. Returns false if the pointer is not owned by this
    // pool or was already released.
    bool release(void *block);

    // True if the pointer lies exactly on a block owned by this pool
    // (whether that block is currently acquired or free).
    bool owns(const void *block) const noexcept;

    std::size_t blockSize() const noexcept { return blockSize_; }
    std::size_t capacity() const noexcept { return blockCount_; }
    std::size_t inUse() const noexcept { return inUse_; }
    std::size_t available() const noexcept { return blockCount_ - inUse_; }
    std::size_t totalBytes() const noexcept { return allocBytes_; }

private:
    void *blockAt(std::size_t index) const noexcept;
    std::size_t indexOf(const void *block) const noexcept;

    std::size_t blockSize_ = 0; // requested usable size per block
    std::size_t stride_ = 0;    // blockSize rounded up to 64B alignment
    std::size_t blockCount_ = 0;
    std::size_t inUse_ = 0;
    std::size_t allocBytes_ = 0; // raw bytes behind the pool
    unsigned char *base_ = nullptr; // raw allocation (may be unaligned)
    unsigned char *first_ = nullptr; // address of the first aligned block
    int64_t head_ = -1;              // index of the first free block
    std::vector<int64_t> nextFree_;  // per-block free-list links
    std::vector<unsigned char> used_;
};

} // namespace fc
