#pragma once
#include <cstdint>
#include <atomic>
#include <array>


template <typename T, size_t Capacity>
class SPSCQueue {
    private:
        alignas(64) std::atomic<size_t> head_{0};
        size_t cached_tail_ = 0;
        alignas(64) std::atomic<size_t> tail_{0};
        size_t cached_head_ = 0;

        alignas(64) std::array<T, Capacity> slots_{};

        static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    
    public: 
        bool try_push(const T& item){
            const size_t tail = tail_.load(std::memory_order_relaxed);
            const size_t next_tail = (tail + 1) & (Capacity - 1);

            if(next_tail == cached_head_){
                cached_head_ = head_.load(std::memory_order_acquire);
                if(next_tail == cached_head_){
                    return false; // queue is full
                }
            }

            slots_[tail] = item;
            tail_.store(next_tail, std::memory_order_release);
            return true;
        }
        bool try_pop(T& item){
            const size_t head = head_.load(std::memory_order_relaxed);
            if(head == cached_tail_){
                cached_tail_ = tail_.load(std::memory_order_acquire);
                if(head == cached_tail_){
                    return false; // queue is empty
                }
            }

            item = slots_[head];
            head_.store((head + 1) & (Capacity - 1), std::memory_order_release);
            return true;
        }
};