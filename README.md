# orderbook-engine

[![CI](https://github.com/ax0080/orderbook-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/ax0080/orderbook-engine/actions/workflows/ci.yml)

A price-time priority limit order book and multi-symbol matching engine in C++17.

- **Order types:** Limit, Market, Stop, and Stop-Limit. Stops trigger on the last trade price, including cascades.
- **Time in force:** GTC, IOC, FOK
- **Modify:** reducing quantity keeps queue priority; changing price or increasing quantity requeues the order and can re-match
- **Self-trade prevention:** CancelTaker / CancelResting / CancelBoth, per trader ID
- **Market data:** trade and fill callbacks, BBO change events, an N-level depth snapshot, and an opt-in incremental L2 feed
- **Pooled orders:** `Order` objects come from a memory pool and are linked into an intrusive FIFO, so queueing an order never allocates. The id index and newly created price levels still use the standard allocator.
- **O(1) cancel:** the order index points straight at the order, which knows its level
- **Reentrant callbacks:** events are queued and delivered after the book is consistent, so listeners can submit or cancel from inside a callback

## Quick start

```bash
git clone https://github.com/ax0080/orderbook-engine.git
cd orderbook-engine
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build              # unit, fuzz and liquibook differential tests
./build/basic_usage                 # walkthrough of every feature
./build/bench_matching_engine
```

Requires CMake 3.20+ and a C++17 compiler. GoogleTest and Google Benchmark are fetched automatically.

```cpp
#include "exchange/matching_engine.h"
#include <cstdio>
using namespace exchange;

struct Printer : OrderListener {
    void on_trade(const Trade& t) override {
        std::printf("%u @ %.2f\n", t.quantity, t.price.to_double());
    }
};

Printer printer;
MatchingEngine engine;                  // default STP: CancelTaker
engine.set_listener(&printer);

engine.submit_order("AAPL", Side::Sell, OrderType::Limit, Price::from_double(100.50), 200);
engine.submit_order("AAPL", Side::Buy, OrderType::Limit,
                    Price::from_double(100.50), 500, TimeInForce::IOC);
// prints "200 @ 100.50"; the other 300 are canceled (IOC)

OrderId bid = engine.submit_order("AAPL", Side::Buy, OrderType::Limit,
                                  Price::from_double(100.00), 100);
engine.modify_order(bid, Price::from_double(100.10), 100);
engine.cancel_order(bid);

// Stop-loss: once a trade prints at or below 95.00, sell 100 at market
engine.submit_stop_order("AAPL", Side::Sell, OrderType::Market,
                         Price::from_double(95.00), Price{0}, 100);

auto top    = engine.get_book("AAPL")->top();              // best bid/ask + size
auto levels = engine.get_book("AAPL")->depth(Side::Sell, 5);
```

## Design

```
MatchingEngine                     one per thread; symbols are sharded across engines
 ├─ books_    symbol  -> OrderBook
 ├─ routes_   OrderId -> OrderBook*    O(1) cancel/modify routing
 └─ pool_     MemoryPool<Order>        shared by all books

OrderBook
 ├─ bids_  std::map<-price, PriceLevel>   begin() = best bid
 ├─ asks_  std::map<+price, PriceLevel>   begin() = best ask
 ├─ index_ OrderId -> Order*
 └─ buy_stops_ / sell_stops_  std::multimap<±stop, StopOrder>   begin() = next to trigger

PriceLevel:  head <-> Order <-> Order <-> tail   (intrusive, FIFO)
             total_qty, count                    (O(1) BBO and depth)
```

| Decision | Why |
|---|---|
| `Price` is `int64` × 10⁴ | Exact comparisons; no floating-point drift in price matching |
| `Order` is `alignas(64)`, 64 bytes | Fits in one cache line; `static_assert` enforces this |
| Intrusive list instead of `std::list` | `std::list` allocates a node for every insert. Intrusive links cost 16 bytes inside the order |
| Bids keyed by negated price | One map type for both sides, so a single `match()` handles buys and sells |
| Pool with free list + bump allocator | Recycles freed slots first (likely still in cache); grows in 4096-order blocks |
| Queued event dispatch | Listeners never see a half-updated book, and re-entrant calls don't recurse into matching |
| Engine owns the listener chain | Terminal events (filled, canceled, rejected) clean up `routes_`, so it tracks only live orders |
| Stops held off-book, outside the pool | Pending stops don't touch the hot path or enlarge `Order`. A loop, not recursion, handles cascades |
| Stop that would trigger on arrival is rejected | Same as most venues. It avoids a hidden market order |
| Depth feed is opt-in | Per-level updates cost about 20% per operation. Consumers that only need BBO don't pay for them |
| Single-threaded | No locks on the hot path; scale by sharding symbols across cores |

| Operation | Cost |
|---|---|
| Add (rest) | O(log M) to find or create the level, M = price levels |
| Cancel | O(1); O(log M) only if the level becomes empty |
| Match | O(1) per fill |
| BBO / top of book | O(1) |
| FOK check | O(levels crossed) |
| Stop trigger check | O(1) per operation, plus O(log S) per triggered stop |

### Event semantics

For each operation, the listener sees events in this order:

1. `on_accept`
2. For each fill: `on_trade`, then `on_fill(taker)`, then `on_fill(maker)`
3. `on_cancel` for any unfilled IOC/FOK/market remainder
4. For each stop triggered by these trades: `on_stop_trigger`, followed by that order's own fills and cancels
5. `on_depth` once for each changed level, if the depth feed is enabled. A quantity of 0 means the level was removed.
6. `on_bbo`, if the top of book changed

`Trade::symbol` is a `string_view` into the book, so no string is copied per trade.

## Testing

| Suite | What it checks |
|---|---|
| Unit tests (70) | Every feature and edge case: FIFO, partial fills, TIF, STP policies, modify priority rules, stop cascades, BBO and depth events, re-entrant listeners, engine routing |
| Fuzz (4 × 50k ops) | Random flow through every feature under each STP policy. After each operation, `OrderBook::validate()` checks links, level totals, indexes and that the book is not crossed. A listener checks each order's lifecycle: fills sum correctly, and no event arrives after a terminal one |
| Differential vs [liquibook](https://github.com/enewhuis/liquibook) (3 × 100k ops) | The same random limit, IOC, FOK, market and cancel stream goes to both engines. After each operation, the fill sequence (taker, maker, price, qty) and full depth must be identical |
| Sanitizers (CI) | The whole suite under ASan and UBSan. The memory pool poisons freed slots, so a use-after-free of a pooled order is reported |

The fuzz and differential tests were checked by planting bugs: breaking a level total, or matching the newest order in a level before older ones. Each planted bug was caught within the first few hundred operations.

## Benchmarks

Setup: i5-12600K, GCC 15.2 `-O3 -march=native`, Windows 10, pinned to one P-core. Each number is the median of 10 runs. Every run uses a populated book and a no-op listener, so every event is still queued and dispatched through a virtual call.

| Scenario | Per operation |
|---|---|
| Add + cancel behind top (200 levels, 2,000 orders) | **29 ns** |
| Add resting + full match at top | **54 ns** |
| Mixed flow: 45% add / 45% cancel / 10% IOC, ~7k resting orders | **61 ns** (16.7M ops/s) |
| Same mixed flow with the L2 depth feed on | **75 ns** |
| Sweep 10 levels (10 adds + 1 sweep) | **87 ns** per order |
| Engine add + match (symbol lookup, routing, timestamp) | **118 ns** |

All numbers are single-threaded. The 12600K is a hybrid CPU, and the same binary runs about 2× slower on an E-core, so pin the process when you benchmark. On Windows use `start /affinity 4 bench_matching_engine.exe`; on Linux use `taskset -c 2`. [benchmarks/](benchmarks/bench_matching_engine.cpp) has the exact setup.

### vs [liquibook](https://github.com/enewhuis/liquibook)

Same workload, same machine, same compiler, median of 10 runs.

| Scenario | orderbook-engine | liquibook | Speedup |
|---|---|---|---|
| Add + match at top | **107 ns** | 158 ns | 1.5× |
| Mixed flow (45% add / 45% cancel / 10% IOC) | **61 ns** | 191 ns | 3.1× |

The gap comes mainly from the memory pool and intrusive linked list: liquibook heap-allocates every order and uses `std::list` nodes. The benchmark source is [benchmarks/bench_vs_liquibook.cpp](benchmarks/bench_vs_liquibook.cpp).

## Layout

```
include/exchange/   types.h  order.h  memory_pool.h  order_book.h  order_listener.h  matching_engine.h
src/                order_book.cpp  matching_engine.cpp
tests/              unit tests, fuzz test, liquibook differential test
benchmarks/         Google Benchmark
examples/           basic_usage.cpp
```

## Roadmap

- Replay historical tick data (e.g. Binance aggTrades) through the engine
- Iceberg orders and post-only orders
- Flat price-indexed array for levels near the touch, instead of `std::map`

## License

MIT
