// Differential test against liquibook (https://github.com/enewhuis/liquibook).
// Both books receive the same random stream of limit (GTC/IOC/FOK), market
// and cancel orders. After every operation the produced fills must be
// identical (taker, maker, price, quantity, in order), and so must the
// aggregated depth on both sides.
//
// Only the feature subset where both engines define the same semantics is
// exercised: liquibook has no self-trade prevention, and its replace and
// stop-order rules differ from this engine's.

#include <gtest/gtest.h>

#include "exchange/order_book.h"

#include <book/order.h>
#include <book/order_book.h>

#include <map>
#include <memory>
#include <random>
#include <tuple>
#include <vector>

namespace lb = liquibook::book;
using namespace exchange;

namespace {

struct Fill {
    OrderId  taker;
    OrderId  maker;
    int64_t  price;
    uint64_t qty;

    bool operator==(const Fill& o) const {
        return std::tie(taker, maker, price, qty) == std::tie(o.taker, o.maker, o.price, o.qty);
    }
};

std::ostream& operator<<(std::ostream& os, const Fill& f) {
    return os << "{taker " << f.taker << ", maker " << f.maker
              << ", px " << f.price << ", qty " << f.qty << "}";
}

// --- liquibook side ---------------------------------------------------------

class LbOrder : public lb::Order {
public:
    LbOrder(OrderId id, bool buy, lb::Price price, lb::Quantity qty)
        : id_(id), buy_(buy), price_(price), qty_(qty) {}

    OrderId      id() const { return id_; }
    bool         is_buy() const override { return buy_; }
    lb::Price    price() const override { return price_; }
    lb::Quantity order_qty() const override { return qty_; }

private:
    OrderId      id_;
    bool         buy_;
    lb::Price    price_;
    lb::Quantity qty_;
};

class LbBook : public lb::OrderBook<LbOrder*> {
public:
    std::vector<Fill> fills;

    void on_fill(LbOrder* const& order, LbOrder* const& matched, lb::Quantity qty,
                 lb::Price price, bool, bool) override {
        fills.push_back({order->id(), matched->id(), static_cast<int64_t>(price), qty});
    }

    // price -> aggregated open quantity, best level first
    std::vector<std::pair<int64_t, uint64_t>> levels(bool buy) const {
        std::vector<std::pair<int64_t, uint64_t>> out;
        for (const auto& [key, tracker] : buy ? bids() : asks()) {
            auto px = static_cast<int64_t>(key.price());
            if (out.empty() || out.back().first != px) out.emplace_back(px, 0);
            out.back().second += tracker.open_qty();
        }
        return out;
    }
};

// --- this engine ------------------------------------------------------------

struct FillRecorder : OrderListener {
    std::vector<Fill> fills;

    void on_trade(const Trade& t) override {
        bool buy_taker = t.aggressor_side == Side::Buy;
        fills.push_back({buy_taker ? t.buyer_order_id : t.seller_order_id,
                         buy_taker ? t.seller_order_id : t.buyer_order_id,
                         t.price.raw, t.quantity});
    }
};

std::vector<std::pair<int64_t, uint64_t>> levels(const OrderBook& book, Side side) {
    std::vector<std::pair<int64_t, uint64_t>> out;
    for (const auto& l : book.depth(side, SIZE_MAX)) out.emplace_back(l.price.raw, l.quantity);
    return out;
}

// ---------------------------------------------------------------------------

class LiquibookDiffTest : public ::testing::TestWithParam<uint64_t> {};

TEST_P(LiquibookDiffTest, SameFillsAndDepth) {
    constexpr int64_t kMid  = 1'000'000;
    constexpr int64_t kTick = 100;

    MemoryPool<Order> pool;
    FillRecorder      ours_listener;
    OrderBook         ours("X", pool, &ours_listener, StpPolicy::None);
    LbBook            theirs;

    std::vector<std::unique_ptr<LbOrder>> lb_orders;   // index = id - 1
    std::mt19937_64 rng(GetParam());
    std::size_t     total_fills = 0;

    for (OrderId id = 1; id <= 100'000; ++id) {
        const bool     buy  = rng() & 1;
        const Side     side = buy ? Side::Buy : Side::Sell;
        const Quantity qty  = 1 + static_cast<Quantity>(rng() % 100);
        const int64_t  px   = kMid + (static_cast<int64_t>(rng() % 41) - 20) * kTick;
        const unsigned r    = static_cast<unsigned>(rng() % 100);

        if (r < 20 && id > 1) {
            // Cancel a random earlier order; it may already be gone in both books.
            OrderId target = 1 + rng() % (id - 1);
            if (LbOrder* o = lb_orders[target - 1].get()) {
                ours.cancel(target);
                theirs.cancel(o);
            }
            lb_orders.push_back(nullptr);   // keep index = id - 1
            ASSERT_EQ(levels(ours, Side::Buy), theirs.levels(true)) << "bids after cancel " << target;
            ASSERT_EQ(levels(ours, Side::Sell), theirs.levels(false)) << "asks after cancel " << target;
            continue;
        }

        OrderType          type = OrderType::Limit;
        TimeInForce        tif  = TimeInForce::GTC;
        lb::OrderConditions cond = lb::oc_no_conditions;
        if (r < 30)      { tif = TimeInForce::IOC; cond = lb::oc_immediate_or_cancel; }
        else if (r < 35) { tif = TimeInForce::FOK; cond = lb::oc_fill_or_kill; }
        else if (r < 42) { type = OrderType::Market; tif = TimeInForce::IOC; cond = lb::oc_immediate_or_cancel; }

        const int64_t limit_px = type == OrderType::Market ? 0 : px;
        lb_orders.push_back(std::make_unique<LbOrder>(id, buy, static_cast<lb::Price>(limit_px), qty));

        ours_listener.fills.clear();
        theirs.fills.clear();
        ours.submit(id, side, type, Price{limit_px}, qty, id, tif);
        theirs.add(lb_orders.back().get(), cond);

        ASSERT_EQ(ours_listener.fills, theirs.fills) << "order " << id;
        ASSERT_EQ(levels(ours, Side::Buy), theirs.levels(true)) << "bids after order " << id;
        ASSERT_EQ(levels(ours, Side::Sell), theirs.levels(false)) << "asks after order " << id;
        total_fills += theirs.fills.size();
    }
    EXPECT_GT(total_fills, 10'000u);
    EXPECT_GT(ours.order_count(), 0u);
}

INSTANTIATE_TEST_SUITE_P(Seeds, LiquibookDiffTest, ::testing::Values(1u, 2u, 3u));

} // namespace
