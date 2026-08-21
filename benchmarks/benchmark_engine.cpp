#include "OrderBook.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

namespace {

Order makeOrder(uint64_t id, Side side, OrderType type, uint64_t price, uint32_t qty, uint64_t owner) {
    Order o{};
    o.orderId = id;
    o.price = price;
    o.timestamp = id;
    o.quantity = qty;
    o.next_idx = UINT32_MAX;
    o.prev_idx = UINT32_MAX;
    o.side = side;
    o.type = type;
    o.ownerId = owner;
    o.tif = TimeInForce::GTC;
    return o;
}

// generated up front so the random number generator never runs inside a timed loop
struct Action {
    bool isCancel;
    Order order;      // valid when isCancel == false
    uint64_t cancelId; // valid when isCancel == true
};

std::vector<Action> generateActions(size_t count, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> action_pick(0, 1); // 0 = submit, 1 = cancel
    std::uniform_int_distribution<uint64_t> price_pick(95, 105);
    std::uniform_int_distribution<uint32_t> qty_pick(1, 20);
    std::uniform_int_distribution<int> side_pick(0, 1);
    std::uniform_int_distribution<uint64_t> owner_pick(1, 4);

    std::vector<Action> actions;
    actions.reserve(count);

    std::vector<uint64_t> submitted_ids;
    uint64_t next_id = 1;

    for (size_t i = 0; i < count; ++i) {
        if (submitted_ids.empty() || action_pick(rng) == 0) {
            Order o = makeOrder(next_id,
                                 side_pick(rng) == 0 ? Side::Buy : Side::Sell,
                                 OrderType::Limit,
                                 price_pick(rng),
                                 qty_pick(rng),
                                 owner_pick(rng));
            actions.push_back(Action{false, o, 0});
            submitted_ids.push_back(next_id);
            ++next_id;
        } else {
            std::uniform_int_distribution<size_t> pick_dist(0, submitted_ids.size() - 1);
            size_t pick = pick_dist(rng);
            actions.push_back(Action{true, Order{}, submitted_ids[pick]});
            submitted_ids.erase(submitted_ids.begin() + pick);
        }
    }

    return actions;
}

void applyAction(OrderBook &book, const Action &action) {
    if (action.isCancel) {
        book.cancel(action.cancelId);
    } else {
        book.submit(action.order);
    }
}

} // namespace

int main() {
    constexpr uint32_t seed = 42;
    constexpr size_t warmupCount = 5000;
    constexpr size_t throughputCount = 2000000;
    constexpr size_t latencyCount = 50000;

    std::vector<Action> warmupActions = generateActions(warmupCount, seed);
    std::vector<Action> throughputActions = generateActions(throughputCount, seed + 1);
    std::vector<Action> latencyActions = generateActions(latencyCount, seed + 2);

    std::cout << "generated " << warmupActions.size() << " warmup, "
              << throughputActions.size() << " throughput, "
              << latencyActions.size() << " latency actions\n";

    constexpr uint32_t poolCapacity = 1000000; // big enough that we won't run out mid-run

    {
        OrderBook book(poolCapacity);

        for (const auto &action : warmupActions) {
            applyAction(book, action); // not timed, just warming things up first
        }

        auto start = std::chrono::steady_clock::now();
        for (const auto &action : throughputActions) {
            applyAction(book, action);
        }
        auto end = std::chrono::steady_clock::now();

        double seconds = std::chrono::duration<double>(end - start).count();
        double perSecond = static_cast<double>(throughputActions.size()) / seconds;

        std::cout << "throughput: " << throughputActions.size() << " actions in "
                  << (seconds * 1000.0) << " ms -> "
                  << static_cast<uint64_t>(perSecond) << " actions/sec\n";
    }

    {
        OrderBook book(poolCapacity);

        for (const auto &action : warmupActions) {
            applyAction(book, action);
        }

        std::vector<int64_t> latenciesNs;
        latenciesNs.reserve(latencyActions.size()); // no reallocations, they'd skew the tail

        for (const auto &action : latencyActions) {
            auto start = std::chrono::steady_clock::now();
            applyAction(book, action);
            auto end = std::chrono::steady_clock::now();
            latenciesNs.push_back(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
        }

        std::sort(latenciesNs.begin(), latenciesNs.end());

        auto percentile = [&](double p) {
            size_t idx = static_cast<size_t>(p * static_cast<double>(latenciesNs.size()));
            return latenciesNs[idx];
        };

        std::cout << "latency p50  = " << percentile(0.50) << " ns\n";
        std::cout << "latency p99  = " << percentile(0.99) << " ns\n";
        std::cout << "latency p999 = " << percentile(0.999) << " ns\n";
    }

    return 0;
}
