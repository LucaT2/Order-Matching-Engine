#pragma once

#include <cstdint>
#include <iostream>
#include "Order.hpp"

struct PriceLevel{
    uint64_t total_volume;
    Order* head_index = nullptr;
    Order* tail_index = nullptr;
};