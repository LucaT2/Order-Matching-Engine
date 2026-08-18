#include "../include/OrderBook.hpp"
#include "OrderBook.hpp"

std::vector<Trade> OrderBook::submit(Order order)
{
    std::vector<Trade> trades;

    if (order.side == Side::Buy)
    {
        auto level_it = sells_.begin();
        while (order.quantity > 0 && level_it != sells_.end())
        {
            PriceLevel &level = level_it->second;
            uint64_t level_price = level_it->first;

            if (order.type == OrderType::Limit && order.price < level_price)
            {
                break; // best ask is above what we're willing to pay — stop matching
            }
            // OrderType::Market keeps going after the break until no more sells left

            uint32_t resting_idx = level.head_idx;
            while (resting_idx != UINT32_MAX && order.quantity > 0)
            {
                Order &resting = pool.at(resting_idx);

                if (resting.ownerId == order.ownerId)
                {
                    resting_idx = resting.next_idx; // self-trade prevention: skip, don't match
                    continue;
                }

                uint32_t traded_qty = std::min(order.quantity, resting.quantity);
                trades.push_back(Trade{
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
                    level.head_idx = next_idx;
                    if (next_idx != UINT32_MAX)
                        pool.at(next_idx).prev_idx = UINT32_MAX;
                    else
                        level.tail_idx = UINT32_MAX; 

                    order_lookup_.erase(resting.orderId);
                    pool.deallocate(resting_idx);
                }

                resting_idx = next_idx;
            }

            if (level.head_idx == UINT32_MAX)
            {
                level_it = sells_.erase(level_it); // level fully drained, remove it and advance
            }
            else
            {
                ++level_it; // only self-owned orders left here — move to the next price level
            }
        }
    }
    else
    {
        auto level_it = bids_.begin();
        while (order.quantity > 0 && level_it != bids_.end())
        {
            PriceLevel &level = level_it->second;
            uint64_t level_price = level_it->first;

            if (order.type == OrderType::Limit && order.price > level_price)
            {
                break;
            }

            uint32_t resting_idx = level.head_idx;
            while (resting_idx != UINT32_MAX && order.quantity > 0)
            {
                Order &resting = pool.at(resting_idx);

                if (resting.ownerId == order.ownerId)
                {
                    resting_idx = resting.next_idx; // self-trade prevention: skip, don't match
                    continue;
                }

                uint32_t traded_qty = std::min(order.quantity, resting.quantity);
                trades.push_back(Trade{
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
                    level.head_idx = next_idx;
                    if (next_idx != UINT32_MAX)
                        pool.at(next_idx).prev_idx = UINT32_MAX;
                    else
                        level.tail_idx = UINT32_MAX;

                    order_lookup_.erase(resting.orderId);
                    pool.deallocate(resting_idx);
                }

                resting_idx = next_idx;
            }

            if (level.head_idx == UINT32_MAX)
            {
                level_it = bids_.erase(level_it); // level fully drained, remove it and advance
            }
            else
            {
                ++level_it; // only self-owned orders left here — move to the next price level
            }
        }
    }

    if (order.type == OrderType::Limit && order.quantity > 0)
    {
        uint32_t idx = pool.allocate();
        pool.at(idx) = order;
        if (order.side == Side::Buy)
        {
            auto it = bids_.find(order.price);
            if (it != bids_.end())
            {
                // level already exists — append the new order to the tail of its FIFO queue
                PriceLevel &level = it->second;

                pool.at(idx).prev_idx = level.tail_idx;
                pool.at(idx).next_idx = UINT32_MAX;
                pool.at(level.tail_idx).next_idx = idx; // old tail -> new order

                level.tail_idx = idx;
                level.total_volume += order.quantity;
            }
            else
            {
                PriceLevel &level = bids_[order.price]; // default-constructs(std::map::operator[] behavior)

                pool.at(idx).prev_idx = UINT32_MAX;
                pool.at(idx).next_idx = UINT32_MAX;

                level.head_idx = idx;
                level.tail_idx = idx;
                level.total_volume = order.quantity;
            }
        }
        else
        {
            auto it = sells_.find(order.price);
            if (it != sells_.end())
            {
                PriceLevel &level = it->second;
                pool.at(idx).prev_idx = level.tail_idx;
                pool.at(idx).next_idx = UINT32_MAX;
                pool.at(level.tail_idx).next_idx = idx;

                level.tail_idx = idx;
                level.total_volume += order.quantity;
            }
            else
            {
                PriceLevel &level = sells_[order.price]; // default-constructs(std::map::operator[] behavior)
                pool.at(idx).prev_idx = UINT32_MAX;
                pool.at(idx).next_idx = UINT32_MAX;

                level.head_idx = idx;
                level.tail_idx = idx;
                level.total_volume = order.quantity;
            }
        }
        order_lookup_[order.orderId] = idx;
    }
    else if (order.type == OrderType::Market && order.quantity > 0)
    {
        //Do nothing, basically discard the market order
    }
    return trades;
}

void OrderBook::cancel(uint64_t order_id)
{
    auto it = order_lookup_.find(order_id);
    if (it == order_lookup_.end()) {
        return; // unknown ID — nothing to cancel
    }

    uint32_t idx = it->second;
    Order order = pool.at(idx);

    if (order.side == Side::Buy) {
        PriceLevel& level = bids_.find(order.price)->second;

        if (order.prev_idx != UINT32_MAX) pool.at(order.prev_idx).next_idx = order.next_idx;
        else                              level.head_idx = order.next_idx;

        if (order.next_idx != UINT32_MAX) pool.at(order.next_idx).prev_idx = order.prev_idx;
        else                              level.tail_idx = order.prev_idx;

        level.total_volume -= order.quantity;

        if (level.head_idx == UINT32_MAX) {
            bids_.erase(order.price); // level fully drained, remove it from the map
        }
    }
    else {
        PriceLevel& level = sells_.find(order.price)->second;

        if (order.prev_idx != UINT32_MAX) pool.at(order.prev_idx).next_idx = order.next_idx;
        else                              level.head_idx = order.next_idx;

        if (order.next_idx != UINT32_MAX) pool.at(order.next_idx).prev_idx = order.prev_idx;
        else                              level.tail_idx = order.prev_idx;

        level.total_volume -= order.quantity;

        if (level.head_idx == UINT32_MAX) {
            sells_.erase(order.price); // level fully drained, remove it from the map
        }
    }

    pool.deallocate(idx);
    order_lookup_.erase(it);
}
