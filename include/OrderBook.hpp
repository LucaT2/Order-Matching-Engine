#pragma once

#include "Order.hpp"
#include "PriceLevel.hpp"
#include "MemoryPool.hpp"
#include "Trade.hpp"

#include <vector>
#include <unordered_map>
#include <iostream>
#include <algorithm>
#include <cstdint>
#include <map>

class OrderBook{
        private:
            MemoryPool<Order> pool;

            // Maps for price levels, both bids and sells
            std::map<uint64_t, PriceLevel, std::greater<uint64_t>> bids_;
            std::map<uint64_t, PriceLevel> sells_;
            
            // Map for cancels
            std::unordered_map<uint64_t, uint32_t> order_lookup_;

        public:
            explicit OrderBook(uint64_t cap): pool(cap){}

            std::vector<Trade> submit(Order order);

            bool canFullyMatch(const Order &order) const;

            void cancel(uint64_t order_id);

            bool checkInvariants() const;

            
    };
