#include <benchmark/benchmark.h>

#include "exchange/order_book.h"

#include <book/order.h>
#include <book/order_book.h>

#include <memory>
#include <random>
#include <vector>

namespace lb = liquibook::book;
using namespace exchange;

// ---------------------------------------------------------------------------
// Listeners / wrappers
// ---------------------------------------------------------------------------

struct NullListener : OrderListener {};

class LbOrder : public lb::Order {
public:
    LbOrder(bool buy, lb::Price price, lb::Quantity qty)
        : buy_(buy), price_(price), qty_(qty) {}
    bool         is_buy()    const override { return buy_; }
    lb::Price    price()     const override { return price_; }
    lb::Quantity order_qty() const override { return qty_; }
private:
    bool         buy_;
    lb::Price    price_;
    lb::Quantity qty_;
};

class LbBook : public lb::OrderBook<LbOrder*> {};

namespace {

constexpr int64_t kMid  = 1'000'000;
constexpr int64_t kTick = 100;

// ---------------------------------------------------------------------------
// Add + Match: rest a sell at mid, match with aggressive buy
// ---------------------------------------------------------------------------

static void BM_Ours_AddMatch(benchmark::State& state) {
    NullListener      listener;
    MemoryPool<Order> pool;
    OrderBook         book("BENCH", pool, &listener);
    OrderId           id = 1;

    for (auto _ : state) {
        book.submit(id, Side::Sell, OrderType::Limit, Price{kMid}, 10, id); ++id;
        book.submit(id, Side::Buy,  OrderType::Limit, Price{kMid}, 10, id); ++id;
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Ours_AddMatch);

static void BM_Liquibook_AddMatch(benchmark::State& state) {
    LbBook book;
    std::vector<std::unique_ptr<LbOrder>> store;
    store.reserve(1 << 20);

    for (auto _ : state) {
        store.push_back(std::make_unique<LbOrder>(false, kMid, 10));
        book.add(store.back().get());
        store.push_back(std::make_unique<LbOrder>(true, kMid, 10));
        book.add(store.back().get());
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Liquibook_AddMatch);

// ---------------------------------------------------------------------------
// Mixed flow: ~45% passive add, ~45% cancel, ~10% IOC
// ---------------------------------------------------------------------------

struct MixOp { uint8_t kind; bool buy; int64_t price; uint32_t qty; uint32_t back; };

static std::vector<MixOp> gen_ops(uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<MixOp> ops(1 << 20);
    for (auto& op : ops) {
        uint32_t r = rng() % 100;
        op.kind  = r < 45 ? 0 : (r < 90 ? 1 : 2);
        op.buy   = rng() & 1;
        int64_t off = 1 + static_cast<int64_t>(rng() % 20);
        op.price = op.buy ? kMid - off * kTick : kMid + off * kTick;
        if (op.kind == 2) op.price = op.buy ? kMid + 5 * kTick : kMid - 5 * kTick;
        op.qty  = 1 + rng() % 100;
        op.back = rng();
    }
    return ops;
}

static void BM_Ours_MixedFlow(benchmark::State& state) {
    auto ops = gen_ops(42);

    NullListener      listener;
    MemoryPool<Order> pool;
    OrderBook         book("BENCH", pool, &listener);
    OrderId           id = 1;

    std::vector<OrderId> live;
    live.reserve(1 << 16);

    std::size_t i = 0;
    for (auto _ : state) {
        const auto& op = ops[i++ & (ops.size() - 1)];
        if (op.kind == 1) {
            if (live.empty()) continue;
            std::size_t k = op.back % live.size();
            book.cancel(live[k]);
            live[k] = live.back();
            live.pop_back();
        } else {
            bool ioc = op.kind == 2;
            book.submit(id, op.buy ? Side::Buy : Side::Sell, OrderType::Limit,
                        Price{op.price}, op.qty, id,
                        ioc ? TimeInForce::IOC : TimeInForce::GTC);
            if (!ioc) live.push_back(id);
            ++id;
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Ours_MixedFlow);

static void BM_Liquibook_MixedFlow(benchmark::State& state) {
    auto ops = gen_ops(42);

    LbBook book;
    std::vector<LbOrder*> all_orders;
    all_orders.reserve(1 << 20);
    std::vector<std::size_t> live;
    live.reserve(1 << 16);

    std::size_t i = 0;
    for (auto _ : state) {
        const auto& op = ops[i++ & (ops.size() - 1)];
        if (op.kind == 1) {
            if (live.empty()) continue;
            std::size_t k = op.back % live.size();
            book.cancel(all_orders[live[k]]);
            live[k] = live.back();
            live.pop_back();
        } else {
            bool ioc = op.kind == 2;
            auto* o = new LbOrder(!op.buy ? false : true,
                                  static_cast<lb::Price>(op.price), op.qty);
            all_orders.push_back(o);
            book.add(o, ioc ? lb::oc_immediate_or_cancel : lb::oc_no_conditions);
            if (!ioc) live.push_back(all_orders.size() - 1);
        }
    }
    for (auto* p : all_orders) delete p;
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Liquibook_MixedFlow);

} // namespace

BENCHMARK_MAIN();
