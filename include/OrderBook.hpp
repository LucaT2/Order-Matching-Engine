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

            // Step best_bid_idx_/best_ask_idx_ inward past levels that have gone
            // empty since they were last used as the scan starting point
            void refreshBestBid();
            void refreshBestAsk();

        public:
            explicit OrderBook(uint64_t cap, uint64_t max_price,
            uint64_t min_price): pool(cap),
            bids_(max_price - min_price + 1),
            sells_(max_price - min_price + 1),
            max_price_(max_price),
            min_price_(min_price),
            best_bid_idx_(UINT64_MAX),
            best_ask_idx_(UINT64_MAX){}

            template <typename Callback>
            void submit(Order order, Callback&& onTrade)
            {
                if (order.type == OrderType::Limit && (order.price < min_price_ || order.price > max_price_)) {
                    return; // price is out of the book's range, can't match
                }

                if (order.tif == TimeInForce::FOK && !canFullyMatch(order)) {
                    return;
                }

                if (order.side == Side::Buy && best_ask_idx_ != UINT64_MAX)
                {
                    uint64_t idx = best_ask_idx_;
                    while (order.quantity > 0 && idx < sells_.size())
                    {
                        PriceLevel &level = sells_[idx];
                        uint64_t level_price = min_price_ + idx;

                        if ( order.tif != TimeInForce::IOC && order.type == OrderType::Limit && order.price < level_price)
                        {
                            break; // price doesn't cross, stop here
                        }
                        // OrderType::Market keeps going after the break until no more sells left

                        uint32_t resting_idx = level.head_idx;
                        while (resting_idx != UINT32_MAX && order.quantity > 0)
                        {
                            Order &resting = pool.at(resting_idx);

                            if (resting.ownerId == order.ownerId)
                            {
                                resting_idx = resting.next_idx; // same owner, skip this one
                                continue;
                            }

                            uint32_t traded_qty = std::min(order.quantity, resting.quantity);
                            onTrade(Trade{
                                order.orderId, resting.orderId,
                                level_price,
                                traded_qty,
                                order.timestamp});

                            order.quantity -= traded_qty;
                            resting.quantity -= traded_qty;
                            level.total_volume -= traded_qty;

                            uint32_t next_idx = resting.next_idx;

                            if (resting.quantity == 0)
                            {
                                // general unlink, resting_idx is not always the head:
                                // an earlier self-owned order could have been skipped, not removed
                                if (resting.prev_idx != UINT32_MAX)
                                    pool.at(resting.prev_idx).next_idx = next_idx;
                                else
                                    level.head_idx = next_idx;

                                if (next_idx != UINT32_MAX)
                                    pool.at(next_idx).prev_idx = resting.prev_idx;
                                else
                                    level.tail_idx = resting.prev_idx;

                                order_lookup_.erase(resting.orderId);
                                pool.deallocate(resting_idx);
                            }

                            resting_idx = next_idx;
                        }
                            ++idx; // only self-owned orders left, try the next level
                    }
                    refreshBestAsk(); // this call's scan may have drained the level best_ask_idx_ pointed at
                }
                else if (order.side == Side::Sell && best_bid_idx_ != UINT64_MAX)
                {
                    uint64_t idx = best_bid_idx_;
                    while (order.quantity > 0) // idx is handled lower in the loop, so we don't need to check it here
                    {
                        PriceLevel &level = bids_[idx];
                        uint64_t level_price = min_price_ + idx;

                        if ( order.tif != TimeInForce::IOC && order.type == OrderType::Limit && order.price > level_price)
                        {
                            break;
                        }

                        uint32_t resting_idx = level.head_idx;
                        while (resting_idx != UINT32_MAX && order.quantity > 0)
                        {
                            Order &resting = pool.at(resting_idx);

                            if (resting.ownerId == order.ownerId)
                            {
                                resting_idx = resting.next_idx; // same owner, skip this one
                                continue;
                            }

                            uint32_t traded_qty = std::min(order.quantity, resting.quantity);
                            onTrade(Trade{
                                resting.orderId, order.orderId,
                                level_price,
                                traded_qty,
                                order.timestamp});

                            order.quantity -= traded_qty;
                            resting.quantity -= traded_qty;
                            level.total_volume -= traded_qty;

                            uint32_t next_idx = resting.next_idx;

                            if (resting.quantity == 0)
                            {
                                // general unlink, resting_idx is not always the head:
                                // an earlier self-owned order could have been skipped, not removed
                                if (resting.prev_idx != UINT32_MAX)
                                    pool.at(resting.prev_idx).next_idx = next_idx;
                                else
                                    level.head_idx = next_idx;

                                if (next_idx != UINT32_MAX)
                                    pool.at(next_idx).prev_idx = resting.prev_idx;
                                else
                                    level.tail_idx = resting.prev_idx;

                                order_lookup_.erase(resting.orderId);
                                pool.deallocate(resting_idx);
                            }

                            resting_idx = next_idx;
                        }
                            if (idx == 0) break; // prevent underflow
                            --idx; // only self-owned orders left, try the next level
                    }
                    refreshBestBid(); // this call's scan may have drained the level best_bid_idx_ pointed at
                }

                if (order.type == OrderType::Limit && order.tif != TimeInForce::IOC && order.quantity > 0)
                {
                    uint32_t idx = pool.allocate();
                    pool.at(idx) = order;

                    if (order.side == Side::Buy)
                    {
                        if (best_bid_idx_ == UINT64_MAX || order.price > min_price_ + best_bid_idx_)
                        {
                            best_bid_idx_ = order.price - min_price_;
                        }
                        PriceLevel &level = bids_[order.price - min_price_];
                        if (level.head_idx != UINT32_MAX)
                        {
                            // level exists already, just add this order to the back of the queue
                            pool.at(idx).prev_idx = level.tail_idx;
                            pool.at(idx).next_idx = UINT32_MAX;
                            pool.at(level.tail_idx).next_idx = idx; // old tail -> new order

                            level.tail_idx = idx;
                            level.total_volume += order.quantity;
                        }
                        else
                        {
                            pool.at(idx).prev_idx = UINT32_MAX;
                            pool.at(idx).next_idx = UINT32_MAX;

                            level.head_idx = idx;
                            level.tail_idx = idx;
                            level.total_volume = order.quantity;
                        }
                    }
                    else // order.side == Side::Sell
                    {
                        if (best_ask_idx_ == UINT64_MAX || order.price < min_price_ + best_ask_idx_)
                        {
                            best_ask_idx_ = order.price - min_price_;
                        }
                        PriceLevel &level = sells_[order.price - min_price_];
                        if (level.head_idx != UINT32_MAX)
                        {
                            pool.at(idx).prev_idx = level.tail_idx;
                            pool.at(idx).next_idx = UINT32_MAX;
                            pool.at(level.tail_idx).next_idx = idx;

                            level.tail_idx = idx;
                            level.total_volume += order.quantity;
                        }
                        else
                        {
                            pool.at(idx).prev_idx = UINT32_MAX;
                            pool.at(idx).next_idx = UINT32_MAX;

                            level.head_idx = idx;
                            level.tail_idx = idx;
                            level.total_volume = order.quantity;
                        }
                    }
                    order_lookup_[order.orderId] = idx;
                }
                // else: leftover from a market order or an IOC order, just drop it, it never rests
            }

            std::vector<Trade> submit(Order order);

            bool canFullyMatch(const Order &order) const;

            void cancel(uint64_t order_id);

            bool checkInvariants() const;

            
    };
