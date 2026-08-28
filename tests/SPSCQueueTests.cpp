#include <gtest/gtest.h>
#include "SPSCQueue.hpp"

#include <cstdint>
#include <thread>

// Capacity=4 sacrifices one slot to disambiguate full/empty, so usable
// capacity is 3 -- see SPSCQueue.hpp's static_assert / design notes.

TEST(SPSCQueue, RejectsPushWhenFull) {
    SPSCQueue<int, 4> q;
    EXPECT_TRUE(q.try_push(10));
    EXPECT_TRUE(q.try_push(20));
    EXPECT_TRUE(q.try_push(30));
    EXPECT_FALSE(q.try_push(40));
}

TEST(SPSCQueue, PopsInFifoOrder) {
    SPSCQueue<int, 4> q;
    q.try_push(10);
    q.try_push(20);
    q.try_push(30);

    int v;
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 10);
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 20);
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 30);
}

TEST(SPSCQueue, PopOnEmptyFails) {
    SPSCQueue<int, 4> q;
    int v;
    EXPECT_FALSE(q.try_pop(v));
}

TEST(SPSCQueue, WrapsAroundAfterDraining) {
    SPSCQueue<int, 4> q;
    q.try_push(10);
    q.try_push(20);
    q.try_push(30);
    ASSERT_FALSE(q.try_push(40));

    int v;
    ASSERT_TRUE(q.try_pop(v)); // removes 10
    ASSERT_TRUE(q.try_pop(v)); // removes 20

    EXPECT_TRUE(q.try_push(40));
    EXPECT_TRUE(q.try_push(50));
    EXPECT_FALSE(q.try_push(60)); // full again

    ASSERT_TRUE(q.try_pop(v)); EXPECT_EQ(v, 30);
    ASSERT_TRUE(q.try_pop(v)); EXPECT_EQ(v, 40);
    ASSERT_TRUE(q.try_pop(v)); EXPECT_EQ(v, 50);
    EXPECT_FALSE(q.try_pop(v));
}

// Real concurrency test: a small capacity forces many wraparounds and many
// full/empty stalls while two real threads race, which is what actually
// exercises the acquire/release pairing (everything above only proves the
// logic is right single-threaded, where no reordering hazard can occur).
TEST(SPSCQueue, TwoThreadsPreserveStrictOrderAndNoLoss) {
    constexpr size_t Capacity = 1024;
    constexpr uint64_t N = 500000;

    SPSCQueue<uint64_t, Capacity> q;

    std::thread producer([&q]() {
        for (uint64_t i = 0; i < N; ++i) {
            while (!q.try_push(i)) {
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
            uint64_t v;
            while (!q.try_pop(v)) {
                std::this_thread::yield();
            }
            EXPECT_EQ(v, expected);
        }
    });

    producer.join();
    consumer.join();
}
