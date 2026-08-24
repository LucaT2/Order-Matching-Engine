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
            std::vector<PriceLevel> bids_;
            std::vector<PriceLevel> sells_;
            
            // Map for cancels
            std::unordered_map<uint64_t, uint32_t> order_lookup_;

            //Tick size is defined in Order.hpp so must calculate prices regarding that
            uint64_t max_price_; 
            uint64_t min_price_;

            //Best buy and sell prices
            uint64_t best_bid_idx_;
            uint64_t best_ask_idx_;

        public:
            explicit OrderBook(uint64_t cap, uint64_t max_price,
            uint64_t min_price): pool(cap),
            bids_(max_price - min_price + 1),
            sells_(max_price - min_price + 1), 
            max_price_(max_price),
            min_price_(min_price),
            best_bid_idx_(UINT64_MAX),
            best_ask_idx_(UINT64_MAX){}

            std::vector<Trade> submit(Order order);

            bool canFullyMatch(const Order &order) const;

            void cancel(uint64_t order_id);

            bool checkInvariants() const;

            
    };
