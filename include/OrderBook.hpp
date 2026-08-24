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

            // One bit per price level (bit i == 1 iff bids_[i]/sells_[i] has
            // resting orders), packed 64 levels per word -- lets refreshBestBid/Ask
            // jump over whole empty 64-level blocks instead of stepping one at a time
            std::vector<uint64_t> bids_occupied_;
            std::vector<uint64_t> sells_occupied_;

            // Map for cancels
            std::unordered_map<uint64_t, uint32_t> order_lookup_;

            //Tick size is defined in Order.hpp so must calculate prices regarding that
            uint64_t max_price_; 
            uint64_t min_price_;

            //Best buy and sell prices
            uint64_t best_bid_idx_;
            uint64_t best_ask_idx_;

            // Step best_bid_idx_/best_ask_idx_ inward past levels that have gone
            // empty since they were last used as the scan starting point
            void refreshBestBid();
            void refreshBestAsk();

            // Occupancy-bitset primitives -- pure bit twiddling on a caller-given
            // word vector, no instance state needed, so these are static
            static void setOccupied(std::vector<uint64_t> &occ, uint64_t idx);
            static void clearOccupied(std::vector<uint64_t> &occ, uint64_t idx);
            // first set bit at index >= from, within [0, levels); UINT64_MAX if none
            static uint64_t nextOccupiedAscending(const std::vector<uint64_t> &occ, uint64_t from, uint64_t levels);
            // first set bit at index <= from; UINT64_MAX if none
            static uint64_t nextOccupiedDescending(const std::vector<uint64_t> &occ, uint64_t from);

        public:
            explicit OrderBook(uint64_t cap, uint64_t max_price,
            uint64_t min_price): pool(cap),
            bids_(max_price - min_price + 1),
            sells_(max_price - min_price + 1),
            bids_occupied_((max_price - min_price + 1 + 63) / 64),
            sells_occupied_((max_price - min_price + 1 + 63) / 64),
            max_price_(max_price),
            min_price_(min_price),
            best_bid_idx_(UINT64_MAX),
            best_ask_idx_(UINT64_MAX){}

            std::vector<Trade> submit(Order order);

            bool canFullyMatch(const Order &order) const;

            void cancel(uint64_t order_id);

            bool checkInvariants() const;

            
    };
