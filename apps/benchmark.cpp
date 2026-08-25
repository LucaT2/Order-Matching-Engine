#include "OrderBook.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <random>
#include <vector>

namespace {

constexpr uint64_t MIN_PRICE = 0;
constexpr uint64_t MAX_PRICE = 29999;      // 30,000 price levels (100-tick-style range) -- spans 469 occupancy-bitset words
constexpr uint64_t POOL_CAPACITY = 2000000; // == NUM_OPS, always enough since submits are at most NUM_OPS
constexpr int NUM_OPS = 2000000;
constexpr int NUM_OWNERS = 50;           // enough distinct owners that self-trade-prevention skips are rare but real

double percentile(const std::vector<double> &sorted_ns, double p)
{
    size_t idx = static_cast<size_t>(p * static_cast<double>(sorted_ns.size() - 1));
    return sorted_ns[idx];
}

} // namespace

int main(int argc, char **argv)
{
    uint32_t seed = argc > 1 ? static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 10)) : 42;

    OrderBook book(POOL_CAPACITY, MAX_PRICE, MIN_PRICE);

    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> action_pick(0, 3);       // 3 of 4 => submit, 1 of 4 => cancel
    std::uniform_int_distribution<uint64_t> price_pick(MIN_PRICE, MAX_PRICE);
    std::uniform_int_distribution<uint32_t> qty_pick(1, 20);
    std::uniform_int_distribution<int> side_pick(0, 1);
    std::uniform_int_distribution<uint64_t> owner_pick(1, NUM_OWNERS);

    std::vector<uint64_t> live_ids;
    live_ids.reserve(NUM_OPS);
    uint64_t next_id = 1;

    std::vector<double> latencies_ns;
    latencies_ns.reserve(NUM_OPS);

    auto wall_start = std::chrono::steady_clock::now();

    for (int i = 0; i < NUM_OPS; ++i)
    {
        if (live_ids.empty() || action_pick(rng) != 3)
        {
            Order o{};
            o.orderId = next_id;
            o.price = price_pick(rng);
            o.timestamp = next_id;
            o.quantity = qty_pick(rng);
            o.next_idx = UINT32_MAX;
            o.prev_idx = UINT32_MAX;
            o.side = side_pick(rng) == 0 ? Side::Buy : Side::Sell;
            o.type = OrderType::Limit;
            o.tif = TimeInForce::GTC;
            o.ownerId = owner_pick(rng);

            auto t0 = std::chrono::steady_clock::now();
            book.submit(o);
            auto t1 = std::chrono::steady_clock::now();
            latencies_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());

            live_ids.push_back(next_id);
            ++next_id;
        }
        else
        {
            std::uniform_int_distribution<size_t> pick_dist(0, live_ids.size() - 1);
            size_t pick = pick_dist(rng);
            uint64_t id = live_ids[pick];
            live_ids[pick] = live_ids.back();
            live_ids.pop_back();

            auto t0 = std::chrono::steady_clock::now();
            book.cancel(id);
            auto t1 = std::chrono::steady_clock::now();
            latencies_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
        }
    }

    auto wall_end = std::chrono::steady_clock::now();
    double wall_seconds = std::chrono::duration<double>(wall_end - wall_start).count();
    double throughput = static_cast<double>(NUM_OPS) / wall_seconds;

    std::sort(latencies_ns.begin(), latencies_ns.end());
    double mean_ns = std::accumulate(latencies_ns.begin(), latencies_ns.end(), 0.0) / static_cast<double>(latencies_ns.size());

    // machine-readable key=value lines, easy to parse when averaging across runs
    std::cout << "seed=" << seed << "\n";
    std::cout << "throughput_ops_per_sec=" << throughput << "\n";
    std::cout << "mean_ns=" << mean_ns << "\n";
    std::cout << "p50_ns=" << percentile(latencies_ns, 0.50) << "\n";
    std::cout << "p99_ns=" << percentile(latencies_ns, 0.99) << "\n";
    std::cout << "p999_ns=" << percentile(latencies_ns, 0.999) << "\n";
    std::cout << "max_ns=" << latencies_ns.back() << "\n";

    return 0;
}
