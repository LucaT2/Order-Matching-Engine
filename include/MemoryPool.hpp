#pragma once

#include <cstdlib>
#include <iostream>
#include <vector>

template<typename T>
class MemoryPool{
    private:
        std::vector<T*> free_list_;
        size_t capacity_;
        size_t next_;
        char* storage_;
    public:
    explicit MemoryPool(size_t capacity): capacity_(capacity), next_(0){
        storage_ = static_cast<char*>(std::malloc(sizeof(T)*capacity));
        free_list_.reserve(cap/4);
    }

};