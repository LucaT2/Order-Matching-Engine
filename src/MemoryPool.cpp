#include "../include/MemoryPool.hpp"
#include <cstring>
#include <new>

template <typename T>
MemoryPool<T>::MemoryPool(size_t capacity)
    : capacity_(capacity), free_head_(0)
{

    storage_ = static_cast<char*>(::operator new(capacity * sizeof(T), std::align_val_t(64)));

    for (uint32_t i = 0; i < capacity - 1; ++i) {
        uint32_t *slot = reinterpret_cast<uint32_t *>(storage_ + (i * sizeof(T)));
        *slot = i + 1; 
    }

    uint32_t *last_slot = reinterpret_cast<uint32_t *>(
        storage_ + ((capacity - 1) * sizeof(T))
    );
    *last_slot = UINT32_MAX;
}

template <typename T>
MemoryPool<T>::~MemoryPool()
{
    if (storage_) {
        ::operator delete(storage_, std::align_val_t(64));
        storage_ = nullptr;
    }
}

template <typename T>
uint32_t MemoryPool<T>::allocate()
{
    if (free_head_ == UINT32_MAX) {
        throw std::runtime_error("Memory pool exhausted");
    }

    uint32_t allocated_idx = free_head_;

    // Read the next free index from inside the slot itself
    uint32_t *next_free_ptr = reinterpret_cast<uint32_t *>(
        storage_ + (allocated_idx * sizeof(T))
    );
    free_head_ = *next_free_ptr;

    return allocated_idx;
}

template <typename T>
void MemoryPool<T>::deallocate(uint32_t idx)
{
    if (idx >= capacity_) {
        throw std::out_of_range("Index out of range");
    }

    // Write the current free_head_ into this slot
    uint32_t *slot = reinterpret_cast<uint32_t *>(
        storage_ + (idx * sizeof(T))
    );
    *slot = free_head_;

    // This slot becomes the new head of the free list
    free_head_ = idx;
}

template <typename T>
T &MemoryPool<T>::at(uint32_t idx)
{
    if (idx >= capacity_) {
        throw std::out_of_range("Index out of range");
    }
    return *reinterpret_cast<T *>(storage_ + (idx * sizeof(T)));
}

template <typename T>
T *MemoryPool<T>::ptr(uint32_t idx)
{
    if (idx >= capacity_) {
        throw std::out_of_range("Index out of range");
    }
    return reinterpret_cast<T *>(storage_ + (idx * sizeof(T)));
}

template <typename T>
void MemoryPool<T>::clear()
{
    free_head_ = 0;

    for (uint32_t i = 0; i < capacity_ - 1; ++i) {
        uint32_t *slot = reinterpret_cast<uint32_t *>(
            storage_ + (i * sizeof(T))
        );
        *slot = i + 1;
    }

    uint32_t *last = reinterpret_cast<uint32_t *>(
        storage_ + ((capacity_ - 1) * sizeof(T))
    );
    *last = UINT32_MAX;
}
