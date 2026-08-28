#include <gtest/gtest.h>
#include "SPSCQueue.hpp"
#include "Order.hpp"

#include <cstdint>
#include <thread>

namespace {
Order makeOrder(uint64_t id, CommandType cmd = CommandType::Submit) {
    Order o{};
    o.orderId = id;
    o.command = cmd;
    return o;
}
} // namespace

// Capacity=4 sacrifices one slot to disambiguate full/empty, so usable
// capacity is 3 -- see SPSCQueue.hpp's static_assert / design notes.

TEST(SPSCQueue, RejectsPushWhenFull) {
    SPSCQueue<Order, 4> q;
    EXPECT_TRUE(q.try_push(makeOrder(10)));
    EXPECT_TRUE(q.try_push(makeOrder(20)));
    EXPECT_TRUE(q.try_push(makeOrder(30)));
    EXPECT_FALSE(q.try_push(makeOrder(40)));
}

TEST(SPSCQueue, PopsInFifoOrder) {
    SPSCQueue<Order, 4> q;
    q.try_push(makeOrder(10));
    q.try_push(makeOrder(20));
    q.try_push(makeOrder(30));

    Order v;
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_EQ(v.orderId, 10u);
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_EQ(v.orderId, 20u);
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_EQ(v.orderId, 30u);
}

TEST(SPSCQueue, PopOnEmptyFails) {
    SPSCQueue<Order, 4> q;
    Order v;
    EXPECT_FALSE(q.try_pop(v));
}

TEST(SPSCQueue, WrapsAroundAfterDraining) {
    SPSCQueue<Order, 4> q;
    q.try_push(makeOrder(10));
    q.try_push(makeOrder(20));
    q.try_push(makeOrder(30));
    ASSERT_FALSE(q.try_push(makeOrder(40)));

    Order v;
    ASSERT_TRUE(q.try_pop(v)); // removes 10
    ASSERT_TRUE(q.try_pop(v)); // removes 20

    EXPECT_TRUE(q.try_push(makeOrder(40)));
    EXPECT_TRUE(q.try_push(makeOrder(50)));
    EXPECT_FALSE(q.try_push(makeOrder(60))); // full again

    ASSERT_TRUE(q.try_pop(v)); EXPECT_EQ(v.orderId, 30u);
    ASSERT_TRUE(q.try_pop(v)); EXPECT_EQ(v.orderId, 40u);
    ASSERT_TRUE(q.try_pop(v)); EXPECT_EQ(v.orderId, 50u);
    EXPECT_FALSE(q.try_pop(v));
}

TEST(SPSCQueue, CommandTypeSurvivesRoundTrip) {
    SPSCQueue<Order, 4> q;
    ASSERT_TRUE(q.try_push(makeOrder(1, CommandType::Cancel)));

    Order v;
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_EQ(v.orderId, 1u);
    EXPECT_EQ(v.command, CommandType::Cancel);
}

// Real concurrency test: a small capacity forces many wraparounds and many
// full/empty stalls while two real threads race, which is what actually
// exercises the acquire/release pairing (everything above only proves the
// logic is right single-threaded, where no reordering hazard can occur).
TEST(SPSCQueue, TwoThreadsPreserveStrictOrderAndNoLoss) {
    constexpr size_t Capacity = 1024;
    constexpr uint64_t N = 500000;

    SPSCQueue<Order, Capacity> q;

    std::thread producer([&q]() {
        for (uint64_t i = 0; i < N; ++i) {
            while (!q.try_push(makeOrder(i))) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&q]() {
        // EXPECT_EQ (not ASSERT_EQ): a failure here must not stop this loop
        // from draining, or the producer could spin forever on a full queue
        // waiting for a pop that will never come, hanging the test instead
        // of failing it.
        for (uint64_t expected = 0; expected < N; ++expected) {
            Order v;
            while (!q.try_pop(v)) {
                std::this_thread::yield();
            }
            EXPECT_EQ(v.orderId, expected);
        }
    });

    producer.join();
    consumer.join();
}
