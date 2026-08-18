#pragma once

#include <cstdint>

enum class Side : uint8_t {
    Buy,
    Sell
};

enum class OrderType : uint8_t {
    Limit,
    Market
};

enum class Action : uint8_t {
    New,
    Cancel,
    Modify
};

struct alignas(64) Order {
    uint64_t orderId;   // 8 bytes: Unique identifier
    uint64_t price;     // 8 bytes: Price in ticks/cents (fixed-point math, no floats)
    uint64_t timestamp; // 8 bytes: Epoch nanoseconds
    uint32_t quantity;  // 4 bytes: Remaining volume to match
    uint32_t next_idx;  // 4 bytes: next Order in PriceLevel
    uint32_t prev_idx;  // 4 bytes: prev Order in PriceLevel
    Side side;          // 1 byte : Buy or Sell
    OrderType type;     // 1 byte : Limit, Market
    uint64_t ownerId; // 8 bytes: id of the owner of the trade
};
