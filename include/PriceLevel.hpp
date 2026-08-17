#pragma once

#include <cstdint>
#include <iostream>
#include "Order.hpp"

struct PriceLevel{
    uint64_t total_volume;
    uint32_t head_idx;
    uint32_t tail_idx;

};