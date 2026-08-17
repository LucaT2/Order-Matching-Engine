#pragma once

#include <cstdint>

enum class Side : uint8_t {
    Buy,
    Sell
};

enum class OrderType : uint8_t {
    Limit,
    Market,
    Cancel
};

struct alignas(32) Order {
    uint64_t orderId;   // 8 bytes: Unique identifier
    uint64_t price;     // 8 bytes: Price in ticks/cents (fixed-point math, no floats)
    uint64_t timestamp; // 8 bytes: Epoch nanoseconds
    uint32_t quantity;  // 4 bytes: Remaining volume to match
    Side side;          // 1 byte : Buy or Sell
    OrderType type;     // 1 byte : Limit, Market, or Cancel
    uint8_t padding[2]{0};
};
