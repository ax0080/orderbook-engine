#include <benchmark/benchmark.h>

#include "exchange/matching_engine.h"
#include "exchange/memory_pool.h"
#include "exchange/order.h"
#include "exchange/order_book.h"
#include "exchange/types.h"

#include <random>
#include <vector>

using namespace exchange;

// Overrides nothing: every event is still queued and dispatched through a
// virtual call, so callback overhead is included in the numbers.
struct NullListener : OrderListener {};

namespace {

constexpr int64_t kMid  = 1'000'000;   // 100.0000
constexpr int64_t kTick = 100;         // 0.01

// A book with `levels` price levels on each side, `per_level` orders each.
void seed(OrderBook& book, OrderId& id, int levels, int per_level) {
    for (int l = 1; l <= levels; ++l) {
        for (int i = 0; i < per_level; ++i) {
            book.submit(id, Side::Buy,  OrderType::Limit, Price{kMid - l * kTick}, 100, id); ++id;
            book.submit(id, Side::Sell, OrderType::Limit, Price{kMid + l * kTick}, 100, id); ++id;
        }
    }
}

} // namespace

// Add a passive order behind the top of a populated book, then cancel it.
static void BM_AddCancel(benchmark::State& state) {
    NullListener      listener;
    MemoryPool<Order> pool;
    OrderBook         book("BENCH", pool, &listener);
    OrderId           id = 1;
    seed(book, id, 100, 10);

    for (auto _ : state) {
        OrderId o = id++;
        book.submit(o, Side::Buy, OrderType::Limit, Price{kMid - 5 * kTick}, 10, o);
        book.cancel(o);
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_AddCancel);

// Rest a sell at the top, then fully match it with an aggressive buy.
static void BM_AddMatch(benchmark::State& state) {
    NullListener      listener;
    MemoryPool<Order> pool;
    OrderBook         book("BENCH", pool, &listener);
    OrderId           id = 1;
    seed(book, id, 100, 10);

    for (auto _ : state) {
        book.submit(id, Side::Sell, OrderType::Limit, Price{kMid}, 10, id); ++id;
        book.submit(id, Side::Buy,  OrderType::Limit, Price{kMid}, 10, id); ++id;
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_AddMatch);

// Build N ask levels, then sweep all of them with one order.
static void BM_Sweep(benchmark::State& state) {
    const int         n = static_cast<int>(state.range(0));
    NullListener      listener;
    MemoryPool<Order> pool;
    OrderBook         book("BENCH", pool, &listener);
    OrderId           id = 1;

    for (auto _ : state) {
        for (int i = 0; i < n; ++i) {
            book.submit(id, Side::Sell, OrderType::Limit, Price{kMid + i * kTick}, 1, id);
            ++id;
        }
        book.submit(id, Side::Buy, OrderType::Limit, Price{kMid + n * kTick},
                    static_cast<Quantity>(n), id);
        ++id;
    }
    state.SetItemsProcessed(state.iterations() * (n + 1));
}
BENCHMARK(BM_Sweep)->Arg(1)->Arg(10)->Arg(100);

// Randomized flow: ~45% passive adds, ~45% cancels of recent adds, ~10%
// marketable IOC orders, prices clustered around the mid. Inputs are
// generated before timing; the book stays at a steady size.
static void BM_MixedFlow(benchmark::State& state) {
    struct Op { uint8_t kind; Side side; int64_t price; Quantity qty; uint32_t back; };

    std::mt19937 rng(42);
    std::vector<Op> ops(1 << 20);
    for (auto& op : ops) {
        uint32_t r = rng() % 100;
        op.kind  = r < 45 ? 0 : (r < 90 ? 1 : 2);
        op.side  = (rng() & 1) ? Side::Buy : Side::Sell;
        int64_t off = 1 + static_cast<int64_t>(rng() % 20);
        op.price = op.side == Side::Buy ? kMid - off * kTick : kMid + off * kTick;
        if (op.kind == 2) op.price = op.side == Side::Buy ? kMid + 5 * kTick : kMid - 5 * kTick;
        op.qty  = 1 + rng() % 100;
        op.back = rng();
    }

    NullListener      listener;
    MemoryPool<Order> pool;
    OrderBook         book("BENCH", pool, &listener);
    book.set_depth_feed(state.range(0) != 0);
    OrderId           id = 1;
    seed(book, id, 20, 5);

    // Each GTC add gets exactly one cancel attempt (it may already be filled).
    std::vector<OrderId> live;
    live.reserve(1 << 16);

    std::size_t i = 0;
    for (auto _ : state) {
        const Op& op = ops[i++ & (ops.size() - 1)];
        if (op.kind == 1) {
            if (live.empty()) continue;
            std::size_t k = op.back % live.size();
            book.cancel(live[k]);
            live[k] = live.back();
            live.pop_back();
        } else {
            bool ioc = op.kind == 2;
            book.submit(id, op.side, OrderType::Limit, Price{op.price}, op.qty, id,
                        ioc ? TimeInForce::IOC : TimeInForce::GTC);
            if (!ioc) live.push_back(id);
            ++id;
        }
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["resting"] = static_cast<double>(book.order_count());
}
BENCHMARK(BM_MixedFlow)->ArgName("depth_feed")->Arg(0)->Arg(1);

// Same as BM_AddMatch, through the multi-symbol engine (symbol lookup,
// routing table, timestamping).
static void BM_EngineAddMatch(benchmark::State& state) {
    NullListener   listener;
    MatchingEngine engine;
    engine.set_listener(&listener);
    const std::string sym = "BENCH";

    for (auto _ : state) {
        engine.submit_order(sym, Side::Sell, OrderType::Limit, Price{kMid}, 10);
        engine.submit_order(sym, Side::Buy,  OrderType::Limit, Price{kMid}, 10);
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_EngineAddMatch);

BENCHMARK_MAIN();
