// Benchmarks OrderBook against the unoptimized-map-implementation branch's own
// benchmarks/benchmark_engine.cpp, which shares this file's structure (warmup phase,
// separate throughput/latency passes, same action counts) so the two are comparable.
// The only real interface difference: this branch's OrderBook takes a price range
// (MIN_PRICE, MAX_PRICE), since bids_/sells_ are bounded flat arrays here rather
// than an unbounded std::map.
#include "OrderBook.hpp"
#include "MemoryPool.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <thread>
#include <vector>
#include <immintrin.h>
#include <intrin.h>

namespace {

// std::chrono::steady_clock on this machine is backed by QueryPerformanceCounter
// running at a fixed 10 MHz (100ns/tick) -- not a hardware limit, Windows normalizes
// QPC to that rate on many configurations. Reading the CPU's own cycle counter
// (RDTSC) bypasses that entirely: ~2 GHz here, so back-to-back reads resolve to
// ~30-40ns instead of the 100ns floor. _mm_lfence() serializes around the read so
// out-of-order execution can't reorder work across the timestamp.
uint64_t rdtscSerialized() {
    _mm_lfence();
    uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}

// One-time calibration: cycles elapsed during a known wall-clock interval gives a
// cycles-per-ns ratio, used afterward to convert cycle deltas to fractional nanoseconds.
double calibrateCyclesPerNs() {
    auto t0 = std::chrono::steady_clock::now();
    uint64_t c0 = rdtscSerialized();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto t1 = std::chrono::steady_clock::now();
    uint64_t c1 = rdtscSerialized();

    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    double cycles = static_cast<double>(c1 - c0);
    return cycles / ns;
}

// 1000 levels: narrow enough that per-tick stepping stays cheap for the array,
// while deep enough that std::map's find() pays a real ~log2(1000)-hop tree
// descent on every insert/cancel.
constexpr uint64_t MIN_PRICE = 0;
constexpr uint64_t MAX_PRICE = 999;

// Non-overlapping bid/ask bands (449 < 550): buys and sells never cross, so every
// operation is a pure insert or cancel rather than a matching sweep. Models passive
// market-maker quoting flow.
constexpr uint64_t BID_BAND_TOP    = 449;
constexpr uint64_t ASK_BAND_BOTTOM = 550;

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

// Generated up front so the RNG never runs inside a timed loop.
struct SimAction {
    bool isCancel;
    Order order;      // valid when isCancel == false
    uint64_t cancelId; // valid when isCancel == true
};

// cancelProbability: 0.5 is the steady-state 50/50 mix; a low value is heavily
// submit-biased, used for an untimed warmup pass that builds a resting population
// before anything is measured.
std::vector<SimAction> generateActions(size_t count, uint32_t seed, double cancelProbability = 0.5) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> action_roll(0.0, 1.0);
    std::uniform_int_distribution<uint64_t> bid_price_pick(MIN_PRICE, BID_BAND_TOP);
    std::uniform_int_distribution<uint64_t> ask_price_pick(ASK_BAND_BOTTOM, MAX_PRICE);
    std::uniform_int_distribution<uint32_t> qty_pick(1, 20);
    std::uniform_int_distribution<int> side_pick(0, 1);
    std::uniform_int_distribution<uint64_t> owner_pick(1, 4);

    std::vector<SimAction> actions;
    actions.reserve(count);

    std::vector<uint64_t> submitted_ids;
    uint64_t next_id = 1;

    for (size_t i = 0; i < count; ++i) {
        bool doCancel = !submitted_ids.empty() && action_roll(rng) < cancelProbability;
        if (!doCancel) {
            Side side = side_pick(rng) == 0 ? Side::Buy : Side::Sell;
            Order o = makeOrder(next_id,
                                 side,
                                 OrderType::Limit,
                                 side == Side::Buy ? bid_price_pick(rng) : ask_price_pick(rng),
                                 qty_pick(rng),
                                 owner_pick(rng));
            actions.push_back(SimAction{false, o, 0});
            submitted_ids.push_back(next_id);
            ++next_id;
        } else {
            std::uniform_int_distribution<size_t> pick_dist(0, submitted_ids.size() - 1);
            size_t pick = pick_dist(rng);
            actions.push_back(SimAction{true, Order{}, submitted_ids[pick]});
            submitted_ids.erase(submitted_ids.begin() + pick);
        }
    }

    return actions;
}

void applyAction(OrderBook &book, const SimAction &action) {
    if (action.isCancel) {
        book.cancel(action.cancelId);
    } else {
        book.submit(action.order);
    }
}

} // namespace

int main() {
    constexpr uint32_t seed = 42;
    // Heavily submit-biased so this untimed pass builds deep per-level queues --
    // with many resting orders per level, a level almost never drains to empty,
    // so refreshBestBid/Ask rarely has to step.
    constexpr size_t warmupCount = 400000;
    constexpr double warmupCancelProbability = 0.02;
    constexpr size_t throughputCount = 2000000;
    // 200k timed calls puts ~200 samples in the top 0.1%, enough for a trustworthy p999.
    constexpr size_t latencyCount = 200000;

    std::vector<SimAction> warmupActions = generateActions(warmupCount, seed, warmupCancelProbability);
    std::vector<SimAction> throughputActions = generateActions(throughputCount, seed + 1);
    std::vector<SimAction> latencyActions = generateActions(latencyCount, seed + 2);

    std::cout << "generated " << warmupActions.size() << " warmup, "
              << throughputActions.size() << " throughput, "
              << latencyActions.size() << " latency actions\n";

    constexpr uint32_t poolCapacity = 2000000;

    // Alloc/dealloc phase: MemoryPool<Order> in isolation. Standalone pool, not
    // OrderBook's internal one (private by design -- submit() owns its own
    // allocation). Same MemoryPool<Order> class on both branches, so this measures
    // the allocator itself, not anything array-vs-map specific.
    {
        MemoryPool<Order> allocPool(throughputCount);
        std::vector<uint32_t> allocated;
        allocated.reserve(throughputCount);

        auto allocStart = std::chrono::steady_clock::now();
        for (size_t i = 0; i < throughputCount; ++i) {
            uint32_t idx = allocPool.allocate();
            allocPool.at(idx) = throughputActions[i].order;
            allocated.push_back(idx);
        }
        auto allocEnd = std::chrono::steady_clock::now();

        double allocUs = std::chrono::duration<double, std::micro>(allocEnd - allocStart).count();
        std::cout << "[ALLOC] " << throughputCount << " orders in " << allocUs << " us  ("
                  << (allocUs * 1000.0 / static_cast<double>(throughputCount)) << " ns/order)\n";

        auto deallocStart = std::chrono::steady_clock::now();
        for (uint32_t idx : allocated) {
            allocPool.deallocate(idx);
        }
        auto deallocEnd = std::chrono::steady_clock::now();

        double deallocUs = std::chrono::duration<double, std::micro>(deallocEnd - deallocStart).count();
        std::cout << "[DEALLOC] " << throughputCount << " orders in " << deallocUs << " us  ("
                  << (deallocUs * 1000.0 / static_cast<double>(throughputCount)) << " ns/order)\n";
    }

    // Throughput: warm up untimed, then time the whole throughput pass as one block
    // (no per-op timing here -- that overhead would skew the very thing being measured).
    {
        OrderBook book(poolCapacity, MAX_PRICE, MIN_PRICE);

        for (const auto &action : warmupActions) {
            applyAction(book, action);
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

    // Latency: separate pass, separate (smaller) action set, every call individually
    // timed via RDTSC (see rdtscSerialized() above) rather than chrono, since chrono's
    // 100ns floor on this machine can't resolve individual sub-100ns operations.
    {
        double cyclesPerNs = calibrateCyclesPerNs();

        OrderBook book(poolCapacity, MAX_PRICE, MIN_PRICE);

        for (const auto &action : warmupActions) {
            applyAction(book, action);
        }

        std::vector<double> latenciesNs;
        latenciesNs.reserve(latencyActions.size());

        for (const auto &action : latencyActions) {
            uint64_t start = rdtscSerialized();
            applyAction(book, action);
            uint64_t end = rdtscSerialized();
            latenciesNs.push_back(static_cast<double>(end - start) / cyclesPerNs);
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
