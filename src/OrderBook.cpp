#include "../include/OrderBook.hpp"
#include "OrderBook.hpp"

std::vector<Trade> OrderBook::submit(Order order)
{
    std::vector<Trade> trades;
    if (order.tif == TimeInForce::FOK && !canFullyMatch(order)) {
        return std::vector<Trade>{};
    }

    if (order.side == Side::Buy)
    {
        auto level_it = sells_.begin();
        while (order.quantity > 0 && level_it != sells_.end())
        {
            PriceLevel &level = level_it->second;
            uint64_t level_price = level_it->first;

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

            if (level.head_idx == UINT32_MAX)
            {
                level_it = sells_.erase(level_it); // nothing left here, drop it and move on
            }
            else
            {
                ++level_it; // only self-owned orders left, try the next level
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

            if (level.head_idx == UINT32_MAX)
            {
                level_it = bids_.erase(level_it); // nothing left here, drop it and move on
            }
            else
            {
                ++level_it; // only self-owned orders left, try the next level
            }
        }
    }

    if (order.type == OrderType::Limit && order.tif != TimeInForce::IOC && order.quantity > 0)
    {
        uint32_t idx = pool.allocate();
        pool.at(idx) = order;
        if (order.side == Side::Buy)
        {
            auto it = bids_.find(order.price);
            if (it != bids_.end())
            {
                // level exists already, just add this order to the back of the queue
                PriceLevel &level = it->second;

                pool.at(idx).prev_idx = level.tail_idx;
                pool.at(idx).next_idx = UINT32_MAX;
                pool.at(level.tail_idx).next_idx = idx; // old tail -> new order

                level.tail_idx = idx;
                level.total_volume += order.quantity;
            }
            else
            {
                PriceLevel &level = bids_[order.price]; // this creates the level automatically since it didn't exist yet

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
                PriceLevel &level = sells_[order.price]; // this creates the level automatically since it didn't exist yet
                pool.at(idx).prev_idx = UINT32_MAX;
                pool.at(idx).next_idx = UINT32_MAX;

                level.head_idx = idx;
                level.tail_idx = idx;
                level.total_volume = order.quantity;
            }
        }
        order_lookup_[order.orderId] = idx;
    }
    else if (order.quantity > 0)
    {
        // leftover from a market order or an IOC order, just drop it, it never rests
    }
    return trades;
}

bool OrderBook::canFullyMatch(const Order &order) const
{
    uint32_t matchable = 0;

    if (order.side == Side::Buy)
    {
        for (auto level_it = sells_.begin(); level_it != sells_.end(); ++level_it)
        {
            const PriceLevel &level = level_it->second;
            uint64_t level_price = level_it->first;

            if (order.type == OrderType::Limit && order.price < level_price)
            {
                break; // price doesn't cross, nothing past this will either
            }

            uint32_t resting_idx = level.head_idx;
            while (resting_idx != UINT32_MAX)
            {
                const Order &resting = pool.at(resting_idx);

                if (resting.ownerId != order.ownerId)
                {
                    matchable += resting.quantity; // don't count our own orders
                    if (matchable >= order.quantity)
                    {
                        return true; // we already have enough, no need to keep checking
                    }
                }

                resting_idx = resting.next_idx;
            }
        }
    }
    else
    {
        for (auto level_it = bids_.begin(); level_it != bids_.end(); ++level_it)
        {
            const PriceLevel &level = level_it->second;
            uint64_t level_price = level_it->first;

            if (order.type == OrderType::Limit && order.price > level_price)
            {
                break;
            }

            uint32_t resting_idx = level.head_idx;
            while (resting_idx != UINT32_MAX)
            {
                const Order &resting = pool.at(resting_idx);

                if (resting.ownerId != order.ownerId)
                {
                    matchable += resting.quantity;
                    if (matchable >= order.quantity)
                    {
                        return true;
                    }
                }

                resting_idx = resting.next_idx;
            }
        }
    }

    return false; // went through every level and it still isn't enough
}

void OrderBook::cancel(uint64_t order_id)
{
    auto it = order_lookup_.find(order_id);
    if (it == order_lookup_.end()) {
        return; // don't know this order, nothing to do
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
            bids_.erase(order.price); // nothing left at this price, remove the level
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
            sells_.erase(order.price); // nothing left at this price, remove the level
        }
    }

    pool.deallocate(idx);
    order_lookup_.erase(it);
}

bool OrderBook::checkInvariants() const
{
    for (auto it = bids_.begin(); it != bids_.end(); ++it)
    {
        const PriceLevel &level = it->second;
        uint64_t summed = 0;
        uint32_t idx = level.head_idx;
        uint32_t prev = UINT32_MAX;
        while (idx != UINT32_MAX)
        {
            const Order &o = pool.at(idx);
            if (o.prev_idx != prev) return false; // back link doesn't match where we came from
            summed += o.quantity;
            prev = idx;
            idx = o.next_idx;
        }
        if (prev != level.tail_idx) return false; // tail doesn't point at the last order we found
        if (summed != level.total_volume) return false; // total_volume drifted from the real sum
    }

    for (auto it = sells_.begin(); it != sells_.end(); ++it)
    {
        const PriceLevel &level = it->second;
        uint64_t summed = 0;
        uint32_t idx = level.head_idx;
        uint32_t prev = UINT32_MAX;
        while (idx != UINT32_MAX)
        {
            const Order &o = pool.at(idx);
            if (o.prev_idx != prev) return false;
            summed += o.quantity;
            prev = idx;
            idx = o.next_idx;
        }
        if (prev != level.tail_idx) return false;
        if (summed != level.total_volume) return false;
    }

    // A crossed price between a bid and an ask is only ok if they share an owner,
    // since self-trade prevention is the one thing allowed to leave a real cross
    // sitting in the book. Any cross between different owners means the matching
    // loop missed a trade it should have made.
    for (auto bid_it = bids_.begin(); bid_it != bids_.end(); ++bid_it)
    {
        uint64_t bid_price = bid_it->first;
        for (auto ask_it = sells_.begin(); ask_it != sells_.end(); ++ask_it)
        {
            if (bid_price < ask_it->first) break; // this and everything after no longer crosses

            for (uint32_t b = bid_it->second.head_idx; b != UINT32_MAX; b = pool.at(b).next_idx)
            {
                for (uint32_t s = ask_it->second.head_idx; s != UINT32_MAX; s = pool.at(s).next_idx)
                {
                    if (pool.at(b).ownerId != pool.at(s).ownerId) return false;
                }
            }
        }
    }

    // every order we're tracking for cancel should point back to a slot with a matching id
    for (const auto &entry : order_lookup_)
    {
        if (pool.at(entry.second).orderId != entry.first) return false;
    }

    return true;
}
