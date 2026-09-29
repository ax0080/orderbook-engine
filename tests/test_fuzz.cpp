// Randomized order flow through every feature (GTC/IOC/FOK, market, stops,
// modify, cancel, STP). After each operation the book's internal invariants
// are checked with OrderBook::validate(), and a listener verifies that every
// order's lifecycle is consistent: fills add up, nothing happens after a
// terminal event, and live orders match what the book holds.

#include <gtest/gtest.h>

#include "exchange/matching_engine.h"

#include <random>
#include <string>
#include <unordered_map>
#include <vector>

using namespace exchange;

namespace {

struct LifecycleChecker : OrderListener {
    struct State {
        Quantity qty    = 0;
        Quantity filled = 0;
        bool     done   = false;
    };

    std::unordered_map<OrderId, State> orders;
    Quantity    next_qty  = 0;   // quantity of the order about to be submitted
    uint64_t    trade_qty = 0;
    uint64_t    fill_qty  = 0;
    uint64_t    triggers  = 0;
    uint64_t    rejects   = 0;
    uint64_t    cancels   = 0;
    std::string error;

    void fail(const std::string& msg, OrderId id) {
        if (error.empty()) error = msg + " (order " + std::to_string(id) + ")";
    }

    State& live(OrderId id, const char* event) {
        State& s = orders[id];
        if (s.done) fail(std::string(event) + " after terminal event", id);
        return s;
    }

    void on_accept(OrderId id) override {
        if (orders.count(id)) fail("accepted twice", id);
        orders[id].qty = next_qty;
    }
    void on_fill(OrderId id, Price, Quantity q, Quantity remaining) override {
        State& s = live(id, "fill");
        s.filled += q;
        fill_qty += q;
        if (s.filled > s.qty || s.qty - s.filled != remaining) fail("remaining mismatch", id);
        if (remaining == 0) s.done = true;
    }
    void on_cancel(OrderId id, Quantity) override { live(id, "cancel").done = true; ++cancels; }
    void on_reject(OrderId id, const char*) override { live(id, "reject").done = true; ++rejects; }
    void on_stop_trigger(OrderId id) override { live(id, "trigger"); ++triggers; }
    void on_modify(OrderId id, Price, Quantity q) override { live(id, "modify").qty = q; }
    void on_trade(const Trade& t) override {
        trade_qty += t.quantity;
        if (t.buyer_order_id == t.seller_order_id) fail("order traded with itself", t.buyer_order_id);
    }

    std::size_t live_count() const {
        std::size_t n = 0;
        for (const auto& [id, s] : orders) n += !s.done;
        return n;
    }
};

constexpr int64_t kMid  = 1'000'000;
constexpr int64_t kTick = 100;

class FuzzTest : public ::testing::TestWithParam<StpPolicy> {};

TEST_P(FuzzTest, InvariantsHoldUnderRandomFlow) {
    LifecycleChecker checker;
    MatchingEngine   engine(GetParam());
    engine.set_listener(&checker);
    engine.set_depth_feed(true);

    std::mt19937_64      rng(1234 + static_cast<int>(GetParam()));
    std::vector<OrderId> ids;
    auto rand_price = [&](int spread) { return Price{kMid + (static_cast<int64_t>(rng() % (2 * spread + 1)) - spread) * kTick}; };
    auto rand_id    = [&]() { return ids.empty() ? OrderId{0} : ids[rng() % ids.size()]; };

    for (int step = 0; step < 50'000; ++step) {
        const Side     side   = (rng() & 1) ? Side::Buy : Side::Sell;
        const Quantity qty    = 1 + static_cast<Quantity>(rng() % 50);
        const TraderId trader = static_cast<TraderId>(rng() % 4);
        const unsigned r      = static_cast<unsigned>(rng() % 100);

        checker.next_qty = qty;
        OrderId id = 0;
        if (r < 40) {
            id = engine.submit_order("X", side, OrderType::Limit, rand_price(15), qty,
                                     TimeInForce::GTC, trader);
        } else if (r < 48) {
            id = engine.submit_order("X", side, OrderType::Limit, rand_price(15), qty,
                                     TimeInForce::IOC, trader);
        } else if (r < 53) {
            id = engine.submit_order("X", side, OrderType::Limit, rand_price(15), qty,
                                     TimeInForce::FOK, trader);
        } else if (r < 58) {
            id = engine.submit_order("X", side, OrderType::Market, Price{0}, qty,
                                     TimeInForce::GTC, trader);
        } else if (r < 66) {
            bool limit = rng() & 1;
            id = engine.submit_stop_order("X", side, limit ? OrderType::Limit : OrderType::Market,
                                          rand_price(20), limit ? rand_price(20) : Price{0},
                                          qty, TimeInForce::GTC, trader);
        } else if (r < 86) {
            engine.cancel_order(rand_id());
        } else {
            OrderId target = rand_id();
            engine.modify_order(target, rand_price(15), 1 + static_cast<Quantity>(rng() % 60));
        }

        if (id != 0) ids.push_back(id);

        const OrderBook* book = engine.get_book("X");
        if (!book) continue;   // no order has reached the book yet
        const std::string problem = book->validate();
        ASSERT_TRUE(problem.empty()) << "step " << step << ": " << problem;
        ASSERT_TRUE(checker.error.empty()) << "step " << step << ": " << checker.error;
        ASSERT_EQ(engine.live_order_count(), book->order_count() + book->stop_order_count())
            << "step " << step;

        if (step % 1000 == 0) {
            ASSERT_EQ(checker.live_count(), engine.live_order_count()) << "step " << step;
        }
    }

    EXPECT_EQ(checker.fill_qty, 2 * checker.trade_qty);
    // The flow must actually reach every path, not just pass vacuously.
    EXPECT_GT(checker.trade_qty, 0u);
    EXPECT_GT(checker.triggers, 0u);
    EXPECT_GT(checker.rejects, 0u);
    EXPECT_GT(checker.cancels, 0u);
}

INSTANTIATE_TEST_SUITE_P(AllStpPolicies, FuzzTest,
                         ::testing::Values(StpPolicy::None, StpPolicy::CancelTaker,
                                           StpPolicy::CancelResting, StpPolicy::CancelBoth));

} // namespace
