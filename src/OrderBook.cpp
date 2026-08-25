#include "../include/OrderBook.hpp"
#include "OrderBook.hpp"

void OrderBook::refreshBestBid()
{
    // only steps past levels that are truly empty (head_idx == UINT32_MAX) --
    // a level with only self-owned-for-some-order orders left is NOT empty,
    // it can still trade with a different owner, so it must stay the best
    while (best_bid_idx_ != UINT64_MAX && bids_[best_bid_idx_].head_idx == UINT32_MAX)
    {
        if (best_bid_idx_ == 0) { best_bid_idx_ = UINT64_MAX; break; }
        --best_bid_idx_;
    }
}

void OrderBook::refreshBestAsk()
{
    while (best_ask_idx_ != UINT64_MAX && sells_[best_ask_idx_].head_idx == UINT32_MAX)
    {
        ++best_ask_idx_;
        if (best_ask_idx_ >= sells_.size()) { best_ask_idx_ = UINT64_MAX; break; }
    }
}

std::vector<Trade> OrderBook::submit(Order order)
{
    std::vector<Trade> trades;

    if (order.type == OrderType::Limit && (order.price < min_price_ || order.price > max_price_)) {
        return std::vector<Trade>{}; // price is out of the book's range, can't match
    }

    if (order.tif == TimeInForce::FOK && !canFullyMatch(order)) {
        return std::vector<Trade>{};
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
    else if (order.quantity > 0)
    {
        // leftover from a market order or an IOC order, just drop
    }
    return trades;
}

bool OrderBook::canFullyMatch(const Order &order) const
{
    uint32_t matchable = 0;

    if (order.side == Side::Buy)
    {
        if (best_ask_idx_ == UINT64_MAX) return false; // no asks at all, can't match anything

        for (uint64_t idx = best_ask_idx_; idx < sells_.size(); ++idx)
        {
            const PriceLevel &level = sells_[idx];
            uint64_t level_price = min_price_ + idx;

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
    else // order.side == Side::Sell
    {
        if (best_bid_idx_ == UINT64_MAX) return false; // no bids at all, can't match anything

        for (uint64_t idx = best_bid_idx_; ; )
        {
            const PriceLevel &level = bids_[idx];
            uint64_t level_price = min_price_ + idx;

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
            if (idx == 0) break; // prevent underflow
            --idx;
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
        PriceLevel& level = bids_[order.price - min_price_];

        if (order.prev_idx != UINT32_MAX) pool.at(order.prev_idx).next_idx = order.next_idx;
        else                              level.head_idx = order.next_idx;

        if (order.next_idx != UINT32_MAX) pool.at(order.next_idx).prev_idx = order.prev_idx;
        else                              level.tail_idx = order.prev_idx;

        level.total_volume -= order.quantity;

        refreshBestBid();
    }
    else {
        PriceLevel& level = sells_[order.price - min_price_];

        if (order.prev_idx != UINT32_MAX) pool.at(order.prev_idx).next_idx = order.next_idx;
        else                              level.head_idx = order.next_idx;

        if (order.next_idx != UINT32_MAX) pool.at(order.next_idx).prev_idx = order.prev_idx;
        else                              level.tail_idx = order.prev_idx;

        level.total_volume -= order.quantity;

        refreshBestAsk();
    }

    pool.deallocate(idx);
    order_lookup_.erase(it);
}

bool OrderBook::checkInvariants() const
{
    for (auto it = bids_.end()-1; ; --it)
    {
        const PriceLevel &level = bids_[it - bids_.begin()];
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
        if (it == bids_.begin()) break; // prevent underflow
    }

    for (auto it = sells_.begin(); it != sells_.end(); ++it)
    {
        const PriceLevel &level = sells_[it - sells_.begin()];
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
        uint64_t bid_price = min_price_ + (bid_it - bids_.begin());
        for (auto ask_it = sells_.begin(); ask_it != sells_.end(); ++ask_it)
        {
            if (bid_price < min_price_ + (ask_it - sells_.begin())) break; // this and everything after no longer crosses

            for (uint32_t b = bids_[bid_it - bids_.begin()].head_idx; b != UINT32_MAX; b = pool.at(b).next_idx)
            {
                for (uint32_t s = sells_[ask_it - sells_.begin()].head_idx; s != UINT32_MAX; s = pool.at(s).next_idx)
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
