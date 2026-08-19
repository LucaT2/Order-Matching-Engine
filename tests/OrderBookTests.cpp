#include <gtest/gtest.h>
#include "OrderBook.hpp"

namespace {

Order makeOrder(uint64_t id, Side side, OrderType type, uint64_t price, uint32_t qty, uint64_t owner,
                TimeInForce tif = TimeInForce::GTC) {
    Order o{};
    o.orderId = id;
    o.price = price;
    o.timestamp = id; // stand-in clock: keeps trades chronologically distinguishable in tests
    o.quantity = qty;
    o.next_idx = UINT32_MAX;
    o.prev_idx = UINT32_MAX;
    o.side = side;
    o.type = type;
    o.ownerId = owner;
    o.tif = tif;
    return o;
}

} // namespace

TEST(OrderBookTest, ExactMatchProducesOneTrade) {
    OrderBook book(16);

    book.submit(makeOrder(1, Side::Sell, OrderType::Limit, 100, 10, /*owner*/ 1));
    auto trades = book.submit(makeOrder(2, Side::Buy, OrderType::Limit, 100, 10, /*owner*/ 2));

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].buyOrderId, 2u);
    EXPECT_EQ(trades[0].sellOrderId, 1u);
    EXPECT_EQ(trades[0].price, 100u);
    EXPECT_EQ(trades[0].quantity, 10u);
}

TEST(OrderBookTest, PartialFillLeavesRestingRemainder) {
    OrderBook book(16);

    book.submit(makeOrder(1, Side::Sell, OrderType::Limit, 100, 10, 1));
    auto trades = book.submit(makeOrder(2, Side::Buy, OrderType::Limit, 100, 4, 2));

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 4u);

    // Resting sell should have exactly 6 left — this only fully clears if that's true.
    auto trades2 = book.submit(makeOrder(3, Side::Buy, OrderType::Limit, 100, 6, 3));
    ASSERT_EQ(trades2.size(), 1u);
    EXPECT_EQ(trades2[0].quantity, 6u);

    // Book should be empty now — nothing left to match.
    auto trades3 = book.submit(makeOrder(4, Side::Buy, OrderType::Limit, 100, 1, 4));
    EXPECT_EQ(trades3.size(), 0u);
}

TEST(OrderBookTest, PriceTimePriorityFIFO) {
    OrderBook book(16);

    book.submit(makeOrder(1, Side::Sell, OrderType::Limit, 100, 5, 1)); // resting first
    book.submit(makeOrder(2, Side::Sell, OrderType::Limit, 100, 5, 2)); // resting second

    auto trades = book.submit(makeOrder(3, Side::Buy, OrderType::Limit, 100, 5, 3));

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].sellOrderId, 1u); // earlier order must fill first
}

TEST(OrderBookTest, SweepsMultipleLevelsAtRestingPrices) {
    OrderBook book(16);

    book.submit(makeOrder(1, Side::Sell, OrderType::Limit, 100, 5, 1));
    book.submit(makeOrder(2, Side::Sell, OrderType::Limit, 101, 5, 2));

    auto trades = book.submit(makeOrder(3, Side::Buy, OrderType::Limit, 101, 10, 3));

    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].price, 100u); // best price consumed first
    EXPECT_EQ(trades[1].price, 101u); // trade executes at the resting price, not the incoming limit
}

TEST(OrderBookTest, CancelRemovesRestingOrder) {
    OrderBook book(16);

    book.submit(makeOrder(1, Side::Sell, OrderType::Limit, 100, 10, 1));
    book.cancel(1);

    auto trades = book.submit(makeOrder(2, Side::Buy, OrderType::Limit, 100, 10, 2));
    EXPECT_EQ(trades.size(), 0u); // canceled order must not be matchable
}

TEST(OrderBookTest, CancelMiddleOrderPreservesRemainingFIFO) {
    OrderBook book(16);

    book.submit(makeOrder(1, Side::Sell, OrderType::Limit, 100, 5, 1));
    book.submit(makeOrder(2, Side::Sell, OrderType::Limit, 100, 5, 2));
    book.submit(makeOrder(3, Side::Sell, OrderType::Limit, 100, 5, 3));

    book.cancel(2); // remove the middle order — exercises the general unlink, not just head/tail

    auto trades = book.submit(makeOrder(4, Side::Buy, OrderType::Limit, 100, 10, 4));

    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].sellOrderId, 1u);
    EXPECT_EQ(trades[1].sellOrderId, 3u); // order 2 correctly skipped
}

TEST(OrderBookTest, CancelUnknownIdIsNoop) {
    OrderBook book(16);
    EXPECT_NO_THROW(book.cancel(999));
}

TEST(OrderBookTest, MarketOrderAgainstEmptyBookIsDiscarded) {
    OrderBook book(16);

    auto trades = book.submit(makeOrder(1, Side::Buy, OrderType::Market, 0, 10, 1));
    EXPECT_EQ(trades.size(), 0u);

    // If the market order had incorrectly rested, this sell would match it.
    auto trades2 = book.submit(makeOrder(2, Side::Sell, OrderType::Limit, 100, 10, 2));
    EXPECT_EQ(trades2.size(), 0u);
}

TEST(OrderBookTest, MarketOrderPartialFillDiscardsRemainder) {
    OrderBook book(16);

    book.submit(makeOrder(1, Side::Sell, OrderType::Limit, 100, 5, 1));
    auto trades = book.submit(makeOrder(2, Side::Buy, OrderType::Market, 0, 10, 2));

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 5u);

    // Remaining 5 of the market buy must NOT rest — confirm nothing sits in bids_ to match.
    auto trades2 = book.submit(makeOrder(3, Side::Sell, OrderType::Limit, 100, 5, 3));
    EXPECT_EQ(trades2.size(), 0u);
}

TEST(OrderBookTest, SelfTradeIsPrevented) {
    OrderBook book(16);

    // Same owner (7) on both sides — must not trade against itself.
    book.submit(makeOrder(1, Side::Sell, OrderType::Limit, 100, 5, 7));
    auto trades = book.submit(makeOrder(2, Side::Buy, OrderType::Limit, 100, 5, 7));
    EXPECT_EQ(trades.size(), 0u);
}

TEST(OrderBookTest, SelfTradeSkipsOwnOrderButStillMatchesOthers) {
    OrderBook book(16);

    // Same price level, FIFO order: owner 7's sell resting first, owner 8's sell second.
    book.submit(makeOrder(1, Side::Sell, OrderType::Limit, 100, 5, 7));
    book.submit(makeOrder(2, Side::Sell, OrderType::Limit, 100, 5, 8));

    // Incoming buy from owner 7: must skip order 1 (self) and match order 2 (other owner) instead.
    auto trades = book.submit(makeOrder(3, Side::Buy, OrderType::Limit, 100, 5, 7));

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].sellOrderId, 2u);
}

TEST(OrderBookTest, FOKFillsCompletelyAcrossMultipleLevelsWhenLiquidityIsSufficient) {
    OrderBook book(16);

    book.submit(makeOrder(1, Side::Sell, OrderType::Limit, 100, 5, 1));
    book.submit(makeOrder(2, Side::Sell, OrderType::Limit, 101, 5, 2));

    auto trades = book.submit(
        makeOrder(3, Side::Buy, OrderType::Limit, 101, 10, 3, TimeInForce::FOK));

    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].price, 100u);
    EXPECT_EQ(trades[1].price, 101u);
}

TEST(OrderBookTest, FOKDiscardsEntirelyAndTouchesNothingWhenLiquidityIsInsufficient) {
    OrderBook book(16);

    book.submit(makeOrder(1, Side::Sell, OrderType::Limit, 100, 5, 1));

    // Only 5 available, but this FOK order wants 10 — must fill NONE of it.
    auto trades = book.submit(
        makeOrder(2, Side::Buy, OrderType::Limit, 100, 10, 2, TimeInForce::FOK));
    EXPECT_EQ(trades.size(), 0u);

    // The resting sell must be completely untouched — a fresh matching buy for
    // the original 5 should still find it there and fill exactly 5.
    auto trades2 = book.submit(makeOrder(3, Side::Buy, OrderType::Limit, 100, 5, 3));
    ASSERT_EQ(trades2.size(), 1u);
    EXPECT_EQ(trades2[0].sellOrderId, 1u);
    EXPECT_EQ(trades2[0].quantity, 5u);
}

TEST(OrderBookTest, IOCPartiallyFillsAndDiscardsRemainder) {
    OrderBook book(16);

    book.submit(makeOrder(1, Side::Sell, OrderType::Limit, 100, 5, 1));
    auto trades = book.submit(
        makeOrder(2, Side::Buy, OrderType::Limit, 100, 10, 2, TimeInForce::IOC));

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 5u);

    // Remaining 5 of the IOC buy must NOT rest — confirm nothing sits in bids_ to match.
    auto trades2 = book.submit(makeOrder(3, Side::Sell, OrderType::Limit, 100, 5, 3));
    EXPECT_EQ(trades2.size(), 0u);
}
