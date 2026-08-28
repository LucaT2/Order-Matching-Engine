#include <gtest/gtest.h>
#include "SPSCQueue.hpp"
#include "OrderBook.hpp"

#include <random>
#include <thread>
#include <vector>

namespace {

// Mixes Submit and Cancel commands into a single deterministic Order stream,
// using Order::command directly -- this is the first real use of that field
// for its intended purpose (SimAction in benchmark_engine.cpp was a stopgap
// wrapper used before the queue needed a single self-describing payload type).
std::vector<Order> generateCommands(size_t count, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> action_roll(0.0, 1.0);
    std::uniform_int_distribution<uint64_t> price_pick(0, 999);
    std::uniform_int_distribution<uint32_t> qty_pick(1, 20);
    std::uniform_int_distribution<int> side_pick(0, 1);
    std::uniform_int_distribution<uint64_t> owner_pick(1, 4);

    std::vector<Order> commands;
    commands.reserve(count);

    std::vector<uint64_t> live_ids;
    uint64_t next_id = 1;

    for (size_t i = 0; i < count; ++i) {
        bool doCancel = !live_ids.empty() && action_roll(rng) < 0.3;
        if (doCancel) {
            std::uniform_int_distribution<size_t> pick_dist(0, live_ids.size() - 1);
            size_t pick = pick_dist(rng);

            Order cmd{};
            cmd.orderId = live_ids[pick];
            cmd.command = CommandType::Cancel;
            commands.push_back(cmd);

            live_ids.erase(live_ids.begin() + pick);
        } else {
            Order cmd{};
            cmd.orderId = next_id;
            cmd.price = price_pick(rng);
            cmd.timestamp = next_id;
            cmd.quantity = qty_pick(rng);
            cmd.next_idx = UINT32_MAX;
            cmd.prev_idx = UINT32_MAX;
            cmd.side = side_pick(rng) == 0 ? Side::Buy : Side::Sell;
            cmd.type = OrderType::Limit;
            cmd.tif = TimeInForce::GTC;
            cmd.ownerId = owner_pick(rng);
            cmd.command = CommandType::Submit;
            commands.push_back(cmd);

            live_ids.push_back(next_id);
            ++next_id;
        }
    }

    return commands;
}

// Baseline: apply the exact same command sequence directly, one call at a
// time, on whatever thread calls this -- no queue involved.
std::vector<Trade> runSynchronously(const std::vector<Order>& commands, OrderBook& book) {
    std::vector<Trade> trades;
    for (const auto& cmd : commands) {
        if (cmd.command == CommandType::Cancel) {
            book.cancel(cmd.orderId);
        } else {
            book.submit(cmd, [&trades](const Trade& t) { trades.push_back(t); });
        }
    }
    return trades;
}

// The actual wiring: a producer thread pushes the same commands into an
// SPSCQueue; a consumer thread -- the only thread that ever touches `book`
// or `trades` -- pops them and dispatches to submit()/cancel().
std::vector<Trade> runThroughQueue(const std::vector<Order>& commands, OrderBook& book) {
    constexpr size_t Capacity = 1024;
    SPSCQueue<Order, Capacity> queue;
    std::vector<Trade> trades;

    std::thread producer([&]() {
        for (const auto& cmd : commands) {
            while (!queue.try_push(cmd)) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&]() {
        for (size_t i = 0; i < commands.size(); ++i) {
            Order cmd;
            while (!queue.try_pop(cmd)) {
                std::this_thread::yield();
            }

            if (cmd.command == CommandType::Cancel) {
                book.cancel(cmd.orderId);
            } else {
                book.submit(cmd, [&trades](const Trade& t) { trades.push_back(t); });
            }
        }
    });

    producer.join();
    consumer.join();
    return trades;
}

} // namespace

// Proves the queue is transparent: routing the identical command sequence
// through two real threads and a lock-free queue produces the exact same
// trades, in the exact same order, as calling OrderBook directly. This only
// holds because SPSC guarantees the consumer sees commands in producer order
// -- if the queue ever reordered or dropped anything, this test would catch it.
TEST(SPSCQueueOrderBookIntegration, ThreadedPipelineMatchesSynchronousExecution) {
    constexpr uint64_t MIN_PRICE = 0;
    constexpr uint64_t MAX_PRICE = 999;
    constexpr uint32_t poolCapacity = 50000;
    constexpr size_t commandCount = 50000;
    constexpr uint32_t seed = 7;

    std::vector<Order> commands = generateCommands(commandCount, seed);

    OrderBook syncBook(poolCapacity, MAX_PRICE, MIN_PRICE);
    std::vector<Trade> syncTrades = runSynchronously(commands, syncBook);

    OrderBook threadedBook(poolCapacity, MAX_PRICE, MIN_PRICE);
    std::vector<Trade> threadedTrades = runThroughQueue(commands, threadedBook);

    EXPECT_TRUE(syncBook.checkInvariants());
    EXPECT_TRUE(threadedBook.checkInvariants());

    ASSERT_EQ(syncTrades.size(), threadedTrades.size());
    for (size_t i = 0; i < syncTrades.size(); ++i) {
        EXPECT_EQ(syncTrades[i].buyOrderId, threadedTrades[i].buyOrderId) << "trade " << i;
        EXPECT_EQ(syncTrades[i].sellOrderId, threadedTrades[i].sellOrderId) << "trade " << i;
        EXPECT_EQ(syncTrades[i].price, threadedTrades[i].price) << "trade " << i;
        EXPECT_EQ(syncTrades[i].quantity, threadedTrades[i].quantity) << "trade " << i;
        EXPECT_EQ(syncTrades[i].timestamp, threadedTrades[i].timestamp) << "trade " << i;
    }
}
