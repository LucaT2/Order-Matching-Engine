# Order Matching Engine

A limit order book and matching engine in C++20, built to measure how data structure choice and memory layout affect latency. Two branches implement the same `OrderBook` API against the same tests and the same benchmark harness:

- `unoptimized-map-implementation`: price levels in a `std::map<uint64_t, PriceLevel>`
- `optimized-array-implementation`: price levels in a flat `std::vector<PriceLevel>` indexed by price offset

## Design

**Price levels.** Bids and asks are flat vectors sized to the book's price range, indexed by `price - min_price_`. Finding a level is an offset calculation, not a tree descent. The cost is that the range must be bounded at construction and memory scales with the range, not with occupied levels.

**Order storage.** Orders live in a slab allocator (`MemoryPool<T>`) that allocates one contiguous block up front and hands out `uint32_t` indices. Free slots form an intrusive free list: the next free index is stored inside the free slot itself, so the list costs no extra memory. Allocation and deallocation measure at about 6.5ns and 2.2ns per order.

**Order linking.** Each price level holds a doubly linked FIFO list of resting orders, linked by pool index rather than pointer. Indices are half the size of pointers on x86-64 and stay valid if backing storage moves.

**Order layout.** `Order` is `alignas(64)` and fills exactly one cache line. Real field data ends at byte 49, leaving room to add fields for free. That is how the queue's command discriminant was added later at no cost.

**Best bid/ask tracking.** Both sides cache the index of the current best price so a sweep starts at the right level. The cached index refreshes lazily, stepping inward only when its level drains.

**Matching.** Price-time priority. Limit and market orders, GTC/IOC/FOK time-in-force orthogonal to order type, self-trade prevention by owner ID.

**Trade reporting.** `submit()` is templated on a callback instead of returning a vector, so the callback type is deduced per call site and inlines into the matching loop. No type erasure, no indirect call, no allocation when an order produces no trades. A `std::vector<Trade> submit(Order)` overload wraps it for simpler callers.

## Performance

Flat array vs. `std::map`, same benchmark on both branches. 1000 price levels, deep per-level queues from a 400,000-action untimed warmup, then 2,000,000 timed actions for throughput and 200,000 individually timed actions for latency. 20 runs each, mean reported. MSYS2 UCRT64 GCC 16.1.0, `-O3`, Release.

Latency uses `RDTSC` (`_mm_lfence()`-serialized, calibrated against `chrono` at startup) rather than `std::chrono` directly. `QueryPerformanceCounter` is normalized to 10 MHz on this machine, so `steady_clock` floors at 100ns, coarser than the operations being measured. Both implementations looked identically fast at p50 until the timing was replaced.

| metric | flat array | `std::map` | difference |
|---|---:|---:|---:|
| throughput (ops/sec) | 28,444,794 | 15,658,318 | 1.82x |
| p50 latency (ns) | 40.58 | 71.64 | 1.77x lower |
| p99 latency (ns) | 215.41 | 239.96 | 1.11x lower |
| p999 latency (ns) | 346.16 | 375.71 | 1.09x lower |
| alloc (ns/order) | 6.584 | 6.601 | tie, same allocator |
| dealloc (ns/order) | 2.397 | 2.286 | tie, same allocator |

### The result is conditional

Those numbers come from passive, non-crossing flow: buys drawn from `[0, 449]`, sells from `[550, 999]`, so nothing crosses and every operation is a pure insert or cancel. That models market-maker quoting, but it is one workload shape, and the array loses the others.

| workload | winner |
|---|---|
| 1000 levels, deep queues, passive/non-crossing flow | array, every metric (1.82x throughput) |
| 1000 levels, saturated, crossing flow | array on throughput (~3%), map on tails (~1.7x) |
| 1000 levels, sparse, crossing flow | map (~12% throughput, ~3x tails) |
| 10,000 levels, saturated, crossing flow | map (~3.1x throughput) |
| 30,000 levels, sparse, crossing flow | map (~4x throughput) |

The array pays for tick distance: walking levels means `++idx` one tick at a time whether or not those ticks hold anything. `std::map` pays for node count: `++iterator` reaches the next occupied level in amortized O(1) regardless of the gap. Sweeps favor the map, inserts and cancels favor the array, where direct indexing beats a roughly 9-hop tree descent. Whichever operation dominates decides the winner.

An early hypothesis went the wrong way. Widening the price range was expected to help the array, since `std::map` is `O(log n)`. It did not: sequential BST iteration is not `O(log n)` per step, so the map's complexity lives in `find()`, not traversal. The array's throughput dropped ~73%, the map's ~12%.

## Concurrency

`SPSCQueue<T, Capacity>` is a lock-free single-producer single-consumer ring buffer feeding order commands to the book from a separate thread. Assuming exactly one producer and one consumer is what removes the need for any CAS loop or lock, and it matches the intended architecture of one matching thread per symbol.

- Fixed power-of-two capacity enforced by `static_assert`, so wrapping is a bitmask rather than a modulo. Storage is a `std::array` allocated once, so there is no heap traffic on the hot path.
- Read and write indices sit on separate 64-byte cache lines. Without that, the producer's write to its index invalidates the consumer's cached copy of the line holding its own index on every operation, despite the two touching unrelated variables.
- Each thread keeps a private, stale copy of the other's index and only re-reads the real atomic when that copy suggests full or empty. In steady state this turns a cross-core round trip per operation into roughly one per `Capacity` operations. The idea comes from the LMAX Disruptor.
- Memory ordering is explicit: `relaxed` reading your own index, `acquire` reading the other thread's, `release` publishing your own. The release/acquire pair guarantees the ordinary non-atomic slot write is visible before the index update advertising it.

One slot is left unused so `head == tail` unambiguously means empty, making usable capacity `Capacity - 1`.

`Order` carries a `CommandType` discriminant (`Submit` or `Cancel`) so one payload type covers both operations and the queue element stays one cache line. The consumer thread is the only thread that touches `OrderBook`, so the book needs no locks and required no changes.

## Tests

23 tests across two binaries.

`unit_tests` covers matching: exact and partial fills, price-time priority, multi-level sweeps, cancels including from the middle of a level's FIFO queue, market orders against an empty book, self-trade prevention, FOK behavior across multiple levels, IOC remainder discarding, best-pointer traversal across empty levels, and a fuzz test asserting invariants after every operation.

`spsc_tests` covers the queue: full and empty detection, FIFO ordering, wraparound, discriminant round-tripping, a two-thread test pushing 500,000 items through a 1024-slot buffer verifying strict order with no loss, and an integration test running 50,000 mixed commands through both a synchronous path and the threaded path, asserting the trade streams match element by element. That equivalence only holds if the queue never reorders, drops, or duplicates.

## Building

CMake 3.20+ and a C++20 compiler. GoogleTest is fetched automatically.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

`CMAKE_BUILD_TYPE` matters. The core library takes its optimization flags from it rather than a per-target `-O3`, so configuring without it silently produces an unoptimized library and benchmark numbers wrong by a factor of several.

```
./build/unit_tests
./build/spsc_tests
./build/benchmark
./build/matching_engine
```

Sanitizer builds, Linux only and mutually exclusive:

```
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DHFT_SANITIZE=ON
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DHFT_TSAN=ON
```

## Layout

```
include/
  Order.hpp        Order struct, side/type/TIF/command enums
  OrderBook.hpp    Book and matching logic (submit() is a template, so it lives here)
  PriceLevel.hpp   FIFO head/tail indices and level volume
  MemoryPool.hpp   Index-based slab allocator
  SPSCQueue.hpp    Lock-free SPSC ring buffer
  Trade.hpp        Execution record
src/               Non-template implementations
tests/             GoogleTest suites
benchmarks/        Throughput and latency harness
apps/              Demo driver
```

## Limitations

Single symbol; the per-symbol sharding the queue anticipates is not implemented. The producer/consumer wiring assumes a known command count, so a live feed would need a shutdown signal. Price range is fixed at construction and array memory scales with it, which is what makes it unsuitable for the wide-range workloads above. No persistence, journaling, or replay.
