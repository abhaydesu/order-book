# Order Book

A limit order book and matching engine in C++20. It matches orders by price-time priority, supports five order types, and is tested under AddressSanitizer and ThreadSanitizer. It sustains about 13.9M operations/sec on a steady-state benchmark.

```
Throughput: 13480874 ops/sec (0.074 s, median of 5 runs)
Latency per operation: p50 42 ns · p99 291 ns · p99.9 417 ns
```

Apple M4, single thread, Release build. See [Benchmark](#benchmark) for the workload and method.

## Features

| Order type | Behaviour |
|---|---|
| `GoodTillCancel` | Rests on the book until it fills or is cancelled. |
| `FillAndKill` | Fills what it can immediately; the rest is cancelled. Rejected if nothing can match. |
| `FillOrKill` | Fills completely right away, or is rejected without trading. |
| `GoodForDay` | Like GoodTillCancel, but cancelled automatically at 16:00 local time. |
| `Market` | Repriced to the worst price on the other side, so it sweeps every level up to its quantity; any remainder then rests at that price. Rejected if that side is empty. |

- **Operations:** add, cancel and modify (cancel and replace, keeping the order type).
- **Snapshots:** `GetOrderInfos()` returns aggregated depth per price level.
- **Thread safety:** every public method takes a mutex. A background thread expires GoodForDay orders and shuts down cleanly when the book is destroyed.

## Design

```cpp
std::map<Price, Level, std::greater<Price>> bids_;   // best (highest) bid first
std::map<Price, Level, std::less<Price>>    asks_;   // best (lowest) ask first
std::unordered_map<OrderId, OrderEntry>     orders_; // id -> order + its position in the level

struct Level { std::list<OrderPointer> orders_; Quantity quantity_; };
```

- **Price levels in a sorted `std::map`.** The best bid and best ask are always at `begin()`. Matching walks levels in price order. Inserting a new level is O(log n).
- **A FIFO `std::list` per level.** This gives time priority within a price. A `std::list` iterator stays valid while other orders are added and removed, so it can be stored.
- **`orders_` maps each id to its list iterator.** Cancelling is a hash lookup plus an O(1) list erase, with no scan of the level.
- **Each level tracks its total remaining quantity.** A FillOrKill check sums levels from the best price and stops at its limit price, without visiting individual orders.

## Build and run

Requires CMake 3.20+ and a C++20 compiler. GoogleTest is downloaded automatically the first time you configure.

```bash
cmake -S . -B build
cmake --build build -j
./build/orderbook          # small demo
./build/orderbook_bench    # benchmark
ctest --test-dir build     # tests
```

The default build type is Release.

## Tests

The suite has 15 tests in two styles.

**Scenario files** (`OrderbookTest/TestFiles/*.txt`). Each file replays a list of actions, then checks the final book:

```
A B GoodTillCancel 100 10 1    # Add: side, type, price, quantity, id
M 1 S 100 10                   # Modify: id, side, price, quantity
C 1                            # Cancel: id
R 1 0 1                        # Expected Result: total orders, bid levels, ask levels
```

**Regression tests** (`OrderbookTest/regression_test.cpp`). There is one test per bug fixed during development. Each one fails, hangs, or triggers a sanitizer error when its fix is reverted:

| Bug | How it showed up |
|---|---|
| A partly filled FillAndKill order cancelled itself through the public `CancelOrder`, re-locking a mutex the caller already held. | Deadlock |
| An emptied price level was erased by a reference to its own key, then that freed key was read again. | ASan: heap-use-after-free |
| Cancel kept using an order entry after erasing it from the map. | ASan: heap-use-after-free |
| `GetOrderInfos` read the book without taking the lock. | TSan: data race |
| The destructor could signal the GoodForDay thread before the thread started waiting, so the signal was lost. | Hang on exit |
| Per-level quantities lived in one hash map keyed by price, shared by both sides. Emptying the bid level at a price wiped the ask quantity at the same price. | FillOrKill wrongly rejected |

Each test also has a 10-second timeout, so a deadlock fails the test instead of hanging CI.

### Sanitizers

```bash
cmake -S . -B build/address -DSANITIZE=address   # AddressSanitizer + UBSan
cmake -S . -B build/thread  -DSANITIZE=thread    # ThreadSanitizer
cmake --build build/address -j && ctest --test-dir build/address
cmake --build build/thread  -j && ctest --test-dir build/thread
```

All 15 tests pass under both builds.

## Benchmark

`bench/benchmark.cpp` replays 1M operations generated in advance against a book seeded with 2,000 resting orders.

- **Realistic cancels.** The stream is generated against a shadow copy of the book, so every cancel and modify targets an order that is actually resting at that moment. The benchmark checks this and prints the count.
- **Steady book size.** Each resting order is equally likely to be cancelled, so cancels rise as the book grows. The book settles at about 1,750 orders instead of draining or growing without bound.
- **Mix:** about 50% GoodTillCancel, 9% FillAndKill, 4% FillOrKill, 1% market, 31% cancel and 5% modify. Limit prices sit from 10 ticks through the mid to 40 ticks behind it, so roughly a fifth of limit orders trade on arrival.
- **Throughput** is timed without per-operation clocks and reported as the median of 5 runs. **Latency** is timed per operation. The clock's resolution (about 41 ns on Apple Silicon) is printed alongside, because the median sits at that floor.
- **Checksum:** an FNV-1a hash over every trade. If an optimisation changes it, the optimisation changed matching behaviour.

```bash
./build/orderbook_bench [operations] [seed]
```

### Optimisation results

Both versions were run on the same workload, alternating runs on the same machine (median of 9 runs each):

| | Before | After |
|---|---|---|
| Throughput | 7.91M ops/sec | **13.93M ops/sec (1.76×)** |
| p99 latency | 458 ns | **291 ns** |
| p99.9 latency | 625 ns | **417 ns** |

Each change was profiled, measured, and checked against the trade checksum:

1. **Removed `trades.reserve(orders_.size())` from matching.** Every incoming order was reserving space for a trade per resting order in the whole book. This was the largest cost in the profile.
2. **One hash lookup per cancel and modify** (`find`, then erase by iterator) instead of `contains`, `at` and then `erase` by key. Shared pointers are also passed by reference to avoid reference-count traffic.
3. **Moved level quantities into the price levels themselves.** This deleted the separate hash map, made the FillOrKill check a short ordered walk, and fixed the FillOrKill bug above. With FillOrKill orders removed from the mix, the checksum is identical before and after, so that fix is the only behaviour change.

I also tried storing a pointer to each order's price level, so cancels would skip the tree lookup. It made no measurable difference, so I reverted it.

## Project layout

```
Orderbook.h / .cpp        matching engine
Order.h, OrderModify.h    order and modify-request types
OrderType.h, Side.h       enums
Trade.h, TradeInfo.h      trade results
LevelInfo.h, OrderbookLevelInfos.h   depth snapshots
Usings.h, Constants.h     shared type aliases
main.cpp                  demo
bench/benchmark.cpp       throughput and latency benchmark
OrderbookTest/            GoogleTest suite and scenario files
```

