# HFT Matching Engine — Learning-Focused Build Roadmap

## Context

You're building a C++ limit order matching engine primarily to get hands-on with low-level C++ (manual memory management, cache-conscious data layout, intrusive data structures, eventually lock-free concurrency), with a secondary goal of ending up with something portfolio-worthy — which for this domain means **correct and realistic**, not just fast-looking. You explicitly want to write the code yourself, so this plan is a sequenced roadmap with the key design decisions and *why* behind each, not code.

Current repo state (E:\HFT_Project) is an early skeleton:
- Not yet a git repo. `build/` (CMake cache + compiled `.exe`) sits uncommitted in the tree.
- `CMakeLists.txt` builds one executable (`matching_engine`) from `apps/main.cpp` only — `src/OrderBook.cpp` isn't even part of the build.
- `apps/main.cpp` just prints a string.
- `include/Order.hpp` — clean 32-byte POD: `orderId`, `price` (u64 ticks, correctly avoiding floats), `timestamp`, `quantity`, `side`, `type`, padding. No linkage fields.
- `include/PriceLevel.hpp` — has `Order* head_index` / `tail_index`, implying an intrusive linked list per price level, but `Order` has no `next`/`prev`, so this can't link up as-is.
- `include/OrderBook.hpp` — empty stub class.
- `src/OrderBook.cpp` — just an include, no implementation.
- `include/MemoryPool.hpp` — has a compile bug (`free_list_.reserve(cap/4)` — `cap` doesn't exist, member is `capacity_`), no `allocate`/`deallocate`/destructor, `storage_` malloc'd but never used correctly.
- Empty `tests/` directory already created (a placeholder, unused so far).

Given your priorities (learn C++, portfolio-correct, in-process only for now, single-symbol/single-threaded first), the plan sequences **correctness before performance before concurrency**, since optimizing or parallelizing a data model that isn't finalized wastes work, and a fast-but-wrong matcher isn't a credible portfolio piece.

---

## Phase 0 — Project hygiene (do this first, low effort)

- `git init`, add a `.gitignore` covering `build/` and compiled artifacts (`*.exe`, `*.obj`) before your first commit — the `build/` directory currently sitting in the tree should never enter history.
- Restructure `CMakeLists.txt` into a **library + app split**: a `matching_engine_core` static library (all of `include/` + `src/OrderBook.cpp`) that `apps/main.cpp` links against, rather than one monolithic executable target. This is what lets you add a test executable and later a benchmark executable that reuse the same core without duplicating compilation — and it fixes the current bug where `src/OrderBook.cpp` isn't even built.
- Decide your warning policy now: keep `/W4 /permissive-` (MSVC) but consider `/WX` (warnings-as-errors) once past the stub stage. Keep `-march=native` gated behind a build option rather than always-on, since it makes binaries non-portable.

---

## Phase 1 — Fix the core data model

Highest-leverage phase — `Order`, `PriceLevel`, and the allocation strategy are load-bearing for everything after.

**Order/PriceLevel linkage (the central decision).** `PriceLevel`'s `head_index`/`tail_index` naming already hints at the right answer: use **index-based intrusive linking**, not raw pointers. Add `next_idx`/`prev_idx` (`uint32_t`) to `Order` instead of `Order* next`/`prev`. Reasons this beats pointers for this project: indices are 4 bytes vs 8 (denser cache lines), they stay valid across pool changes, they serialize trivially if you ever log/replay state, and a sentinel (`UINT32_MAX`) cleanly means "null." This is the standard pattern production order books use and pairs directly with an index-based memory pool (Phase 2). Rename `PriceLevel::head_index`/`tail_index` to be indices too, consistent with the naming that's already there.

**Fixed-point price.** Keep `uint64_t` ticks (correct choice already — no floats in the matching path, ever). Decide: `Order.price` stays an opaque tick count, and tick size / valid price range is configuration owned by the `OrderBook`/symbol, not baked into the `Order` type — this keeps one engine binary able to host symbols with different tick regimes.

**Order ID generation.** Not implemented yet. Use a plain monotonic counter owned by whatever submits orders to the book (not the `OrderBook` itself) — keep `OrderBook` treating `orderId` as an opaque key it's given, not something it mints. This keeps the matching core decoupled from ingestion, which matters later when you add a real ingestion layer.

**Split `Cancel` out of `OrderType`.** Cancel is an action, not an order type — it doesn't have a meaningful "price"/"quantity" the way Limit/Market do. Replace with either a small `Action` enum (`New`/`Cancel`/`Modify`) or separate `OrderBook` methods (`submit`/`cancel`/`modify`). This avoids ambiguity bugs about what fields mean on a cancel "order."

**Add an owner/participant field** while you're already revising `Order` — needed later for self-trade prevention (Phase 3), cheap to add now, awkward to retrofit.

---

## Phase 2 — Fix MemoryPool into a real slab allocator

Why a custom pool matters here specifically: `new`/`std::allocator` have unpredictable worst-case latency (heap metadata walks, possible syscalls) and no layout control — both directly hurt tail latency, which is the metric that actually matters in this domain.

- Fix the immediate bug (`cap` → `capacity_`), then go further: implement real `allocate()`/`deallocate()`, a destructor that frees `storage_`, and correct construction (placement-new via `std::construct_at`, since `T` is currently trivial but the pool should be correct generically).
- Switch the free list from `std::vector<T*>` to an **index-based intrusive free list**: store the "next free slot" index inside each free slot's own unused storage, avoiding a separate free-list container entirely. This is the classic slab-allocator trick and matches the index-based linking from Phase 1.
- Fixed capacity, allocated once (`std::aligned_alloc`/`_aligned_malloc` for cache-line alignment), never grows. Decide explicitly what "pool exhausted" does (reject with an error — never grow on the hot path, since growth invalidates live indices).
- Keep it generic/templated so it can later back a `Trade`/fill-report pool too.
- Write pool-only unit tests before touching matching logic: allocate-to-exhaustion, deallocate-and-reuse, alignment checks.

---

## Phase 3 — Single-symbol matching engine (the real MVP)

Where `OrderBook.hpp` stops being an empty stub. Still single-threaded, single-symbol, Limit + Market only, no networking — the goal is a genuinely correct, testable engine.

**Price-level container — the central tradeoff, and where to start:**
- `std::map<uint64_t, PriceLevel>` (bids descending, asks ascending): simple, correct, no pre-sizing needed, O(log n) level lookup/insert, but pointer-chasing and per-level node allocation.
- Flat array indexed by price tick, bounded to a configured range, with an occupancy bitset to skip empty levels: O(1) level lookup, cache-friendly, no dynamic allocation for book structure — closer to what real low-latency books do, but needs a bounded price range and more supporting machinery.

**Recommendation: build with `std::map` first** to get correctness and tests in place fast, then treat the swap to a flat array as an explicit, isolated Phase 5 milestone once you have a test suite to diff against. Don't let the "proper" flat-array design block getting a working, tested engine.

**Within a level**, the FIFO queue is the Phase 1 index-based intrusive list — true O(1) enqueue/dequeue/arbitrary-removal (needed for O(1) cancel), which a `std::deque`/`vector` of orders per level wouldn't give you.

**Matching algorithm (price-time priority):** walk the opposite side from best price while the incoming order can still cross and has remaining quantity; consume resting orders FIFO per level, emit a `Trade` per match (define `Trade{buyOrderId, sellOrderId, price, quantity, timestamp}`), free fully-filled orders back to the pool, rest any remainder (Limit) or decide-and-document a policy for unfilled Market remainder (reject vs. partial — pick one, it's genuinely venue-specific and needs an explicit choice).

**Cancel:** needs an `unordered_map<orderId, location>` since cancels arrive by ID, not book position — this is the one hash map indirection that's unavoidable even in an otherwise array/index-based design. O(1) unlink from the intrusive list, update level `total_volume`, free the pool slot.

**Self-trade prevention:** worth including early using the owner field from Phase 1 — it's a common correctness edge case and a good signal in a portfolio review.

**Milestone:** by the end of this phase, wire `apps/main.cpp` to a real `OrderBook` — a scripted sequence of add/cancel orders producing trade output. This is your first true "it works end-to-end" checkpoint, and should happen well before any performance or concurrency work. Build the correctness test suite (Phase 7 topics) *alongside* this phase, not after — price-time-priority bugs are easy to miss by inspection.

---

## Phase 4 — Order types and time-in-force

Layer on after Phase 3 is solid and tested:

- Model **IOC/FOK/GTC as a `TimeInForce` field**, orthogonal to `OrderType::Limit` — not new order types. GTC is what Phase 3 already does. IOC: match what you can, discard the remainder instead of resting. FOK: needs an all-or-nothing check — do a dry-run walk to confirm full fill is possible *before* mutating any state, rather than executing-then-rolling-back.
- **Stop/stop-limit orders**: real added complexity (a separate pending-stops structure watched against last-trade price, trigger semantics to define). Worth it for portfolio depth but treat as optional/stretch, after the core + performance work, since it's easy to get subtly wrong (triggered-stop cascades).
- **Explicitly out of scope** (state this in your README so it reads as a scoping decision, not a gap): iceberg/hidden orders, pegged orders, multi-leg orders, full FIX conformance.

---

## Phase 5 — Performance engineering

Only start this once Phase 7's correctness tests exist — this phase changes internals under a behavior contract you can verify against, not before.

- **Cache-line sizing**: after Phase 1 adds `next_idx`/`prev_idx` to `Order`, revisit its total size — target exactly 32 or 64 bytes (half/whole cache line), not an awkward in-between that straddles cache-line boundaries.
- **Zero heap allocation on the hot path**: audit `Trade` reporting and any per-level allocation (relevant if still on `std::map`) for allocation outside the rare "first order at a brand-new price" case. This is the trigger point for actually doing the flat-array swap from Phase 3 if profiling shows it matters.
- **Branch prediction / SIMD**: lower priority here — the matching loop's branches are naturally predictable with realistic order flow, and a one-order-at-a-time linked-list matcher isn't naturally SIMD-friendly. Don't force it; a batch occupancy-bitset scan for the next non-empty level is the one place SIMD could plausibly help.
- **Why single-thread-per-symbol beats locking** (concept to internalize before Phase 6): a single symbol's book has no real internal parallelism — matches must be strictly ordered — so parallelism only exists *across* symbols. Mutexes hurt tail latency specifically (scheduler involvement, cache-line bouncing of the lock word), which is why the standard design gives one thread exclusive ownership of a symbol's book instead.

---

## Phase 6 — Concurrency (deferred per your single-threaded-first preference, sketch only)

Not a near-term milestone, but worth having the target shape in mind so Phase 3's `OrderBook` API doesn't accidentally make this harder later: keep all mutation entering through a small number of methods (submit/cancel/modify) rather than exposing internal state, so a future single-writer queue in front of the book is a plumbing change, not a redesign. When you do get here: single thread per symbol, SPSC ring buffer for ingestion (lock-free by construction since there's exactly one producer/consumer), and the Disruptor pattern as a stretch goal once the simple version works.

---

## Phase 7 — Testing (start during Phase 3, not after)

- Pick GoogleTest or Catch2, wire through CTest (`tests/` directory already exists, currently empty — this is where it gets used).
- Correctness suite: exact match, partial fill (one side / both sides), multi-level walk-through, price-time priority (same price, earlier order fills first), self-trade prevention, cancel of resting order (verify `total_volume` bookkeeping), cancel of nonexistent ID (defined behavior, no crash), IOC/FOK edge cases, market order against empty/thin book.
- Property/fuzz testing: random add/cancel sequences checking invariants — volume conservation, book never crosses, priority never violated, no dangling pool indices (run under ASan to catch use-after-free on canceled orders).
- Sanitizer build: a CMake option for `-fsanitize=address,undefined`, run against the full suite — specifically valuable here since the design is hand-rolled pointer/index manipulation (intrusive lists, pool) that's exactly what ASan/UBSan catch bugs in.
- Benchmark harness (once Phase 5 starts): throughput (orders/sec) plus **p50/p99/p999 latency**, not just averages — averages hide the pathological cases (deep level walks, near-exhaustion) that matter most for an HFT-flavored story. Google Benchmark for throughput; a manual timer buffer + percentile post-processing for latency distribution.

---

## Suggested build order (critical path)

1. **Phase 0 + Phase 1**: git init, CMake library split, fix `Order`/`PriceLevel` linkage and finalize the struct shapes. No behavior yet — just a compiling, git-tracked, correctly-shaped skeleton.
2. **Phase 2**: correct, unit-tested `MemoryPool` in isolation.
3. **Phase 3**: the actual matching engine, `std::map`-based levels, wired into `apps/main.cpp` for a visible end-to-end demo — build tests alongside.
4. **Phase 4 (IOC/FOK/GTC only)**: rounds out realism cheaply.
5. **Phase 7 (fuzz + sanitizer)**: once there's a stable feature set worth fuzzing.
6. **Phase 5**: performance pass using the test suite as a regression oracle — this is where the `std::map` → flat-array decision gets made for real, plus the benchmark harness.
7. **Phase 6**: concurrency, if/when you want to pick it back up.
8. **Phase 4 (stop orders)** and CI polish: optional, last, portfolio-completeness items.

The non-negotiable core is Phases 1–3 plus correctness testing — everything after Phase 5 is turning a *correct* engine into a *fast* one, and shouldn't start before the data model and matching logic are fixed and tested.

## Verification

No code will exist to verify until you build it, but as each phase lands:
- Phase 1–2: pool unit tests pass, project compiles clean under `/W4` (or `/WX`) with no warnings.
- Phase 3: `apps/main.cpp` demo produces correct trades for a hand-checked scripted order sequence; correctness test suite (Phase 7) passes.
- Phase 5: benchmark numbers (throughput + latency percentiles) recorded before/after each optimization, with the Phase 7 suite still green after each change — never accept a "faster" change that breaks a correctness test.
