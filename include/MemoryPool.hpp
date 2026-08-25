#pragma once

#include <cstdlib>
#include <cstdint>
#include <stdexcept>
#include <cassert>

template <typename T>
class MemoryPool
{
private:
    char *storage_;
    size_t capacity_;
    uint32_t free_head_;

public:
    explicit MemoryPool(size_t capacity);
    ~MemoryPool();

    void clear();

    uint32_t allocate();
    void deallocate(uint32_t idx);

    T &at(uint32_t idx);             // Access object by index
    const T &at(uint32_t idx) const; // Read-only access, usable from const methods
    T *ptr(uint32_t idx);            // Get pointer to object

    size_t capacity() const { return capacity_; }
    bool is_exhausted() const { return free_head_ == UINT32_MAX; }
};

// Template implementations
#include "../src/MemoryPool.cpp"