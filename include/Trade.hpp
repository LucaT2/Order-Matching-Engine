#pragma once

#include <cstdlib>
#include <cstdint>

struct Trade{
    uint64_t buyOrderId;
    uint64_t sellOrderId;
    uint64_t price;
    uint64_t quantity;
    uint64_t timestamp;
};