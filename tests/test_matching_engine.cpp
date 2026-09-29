#include <gtest/gtest.h>

#include "exchange/matching_engine.h"
#include "exchange/order_listener.h"
#include "exchange/types.h"

#include <vector>

using namespace exchange;

struct EngineListener : OrderListener {
    std::vector<Trade>   trades;
    std::vector<OrderId> accepted;
    std::vector<OrderId> canceled;

    void on_accept(OrderId id) override { accepted.push_back(id); }
    void on_fill(OrderId, Price, Quantity, Quantity) override {}
    void on_cancel(OrderId id, Quantity) override { canceled.push_back(id); }
    void on_reject(OrderId, const char*) override {}
    void on_trade(const Trade& t) override { trades.push_back(t); }

    void clear() {
        trades.clear();
        accepted.clear();
        canceled.clear();
    }
};

class MatchingEngineTest : public ::testing::Test {
protected:
    EngineListener listener;
    MatchingEngine engine;

    void SetUp() override { engine.set_listener(&listener); }
};

// ---------------------------------------------------------------------------
// Basic engine tests
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, SubmitReturnsNonZeroId) {
    auto id = engine.submit_order("AAPL", Side::Buy, OrderType::Limit,
                                  Price::from_double(150.0), 100);
    EXPECT_NE(id, 0u);
}

TEST_F(MatchingEngineTest, UniqueIds) {
    auto id1 = engine.submit_order("AAPL", Side::Buy, OrderType::Limit,
                                   Price::from_double(150.0), 100);
    auto id2 = engine.submit_order("AAPL", Side::Buy, OrderType::Limit,
                                   Price::from_double(150.0), 100);
    EXPECT_NE(id1, id2);
}

TEST_F(MatchingEngineTest, CrossSymbolIsolation) {
    engine.submit_order("AAPL", Side::Sell, OrderType::Limit,
                        Price::from_double(150.0), 100);
    engine.submit_order("MSFT", Side::Buy, OrderType::Limit,
                        Price::from_double(150.0), 100);

    // Should NOT match across symbols
    EXPECT_TRUE(listener.trades.empty());

    auto* aapl = engine.get_book("AAPL");
    auto* msft = engine.get_book("MSFT");
    ASSERT_NE(aapl, nullptr);
    ASSERT_NE(msft, nullptr);
    EXPECT_EQ(aapl->order_count(), 1u);
    EXPECT_EQ(msft->order_count(), 1u);
}

TEST_F(MatchingEngineTest, MatchWithinSymbol) {
    engine.submit_order("AAPL", Side::Sell, OrderType::Limit,
                        Price::from_double(150.0), 50);
    listener.clear();

    engine.submit_order("AAPL", Side::Buy, OrderType::Limit,
                        Price::from_double(150.0), 50);

    ASSERT_EQ(listener.trades.size(), 1u);
    EXPECT_EQ(listener.trades[0].symbol, "AAPL");
    EXPECT_EQ(listener.trades[0].quantity, 50u);
}

TEST_F(MatchingEngineTest, CancelOrderAcrossBooks) {
    auto id = engine.submit_order("TSLA", Side::Buy, OrderType::Limit,
                                  Price::from_double(200.0), 10);
    EXPECT_TRUE(engine.cancel_order(id));
    EXPECT_FALSE(engine.cancel_order(id)); // already canceled

    auto* book = engine.get_book("TSLA");
    ASSERT_NE(book, nullptr);
    EXPECT_EQ(book->order_count(), 0u);
}

TEST_F(MatchingEngineTest, CancelNonexistentOrder) {
    EXPECT_FALSE(engine.cancel_order(99999));
}

TEST_F(MatchingEngineTest, GetBookNullForUnknownSymbol) {
    EXPECT_EQ(engine.get_book("NOPE"), nullptr);
}

TEST_F(MatchingEngineTest, MultipleSymbolsMultipleTrades) {
    engine.submit_order("AAPL", Side::Sell, OrderType::Limit,
                        Price::from_double(150.0), 100);
    engine.submit_order("MSFT", Side::Sell, OrderType::Limit,
                        Price::from_double(300.0), 50);
    listener.clear();

    engine.submit_order("AAPL", Side::Buy, OrderType::Limit,
                        Price::from_double(150.0), 100);
    engine.submit_order("MSFT", Side::Buy, OrderType::Limit,
                        Price::from_double(300.0), 50);

    EXPECT_EQ(listener.trades.size(), 2u);
    EXPECT_EQ(engine.get_book("AAPL")->order_count(), 0u);
    EXPECT_EQ(engine.get_book("MSFT")->order_count(), 0u);
}

TEST_F(MatchingEngineTest, MarketOrderThroughEngine) {
    engine.submit_order("AAPL", Side::Sell, OrderType::Limit,
                        Price::from_double(150.0), 100);
    listener.clear();

    engine.submit_order("AAPL", Side::Buy, OrderType::Market,
                        Price{0}, 100);

    ASSERT_EQ(listener.trades.size(), 1u);
    EXPECT_EQ(listener.trades[0].quantity, 100u);
    EXPECT_EQ(engine.get_book("AAPL")->order_count(), 0u);
}

// ---------------------------------------------------------------------------
// Routing table
// ---------------------------------------------------------------------------

TEST_F(MatchingEngineTest, CancelRoutesToOwningBook) {
    auto aapl = engine.submit_order("AAPL", Side::Buy, OrderType::Limit,
                                    Price::from_double(150.0), 10);
    auto msft = engine.submit_order("MSFT", Side::Buy, OrderType::Limit,
                                    Price::from_double(300.0), 10);

    EXPECT_TRUE(engine.cancel_order(msft));
    EXPECT_EQ(engine.get_book("MSFT")->order_count(), 0u);
    EXPECT_EQ(engine.get_book("AAPL")->order_count(), 1u);
    EXPECT_TRUE(engine.cancel_order(aapl));
}

TEST_F(MatchingEngineTest, RoutesTrackOnlyLiveOrders) {
    engine.submit_order("AAPL", Side::Sell, OrderType::Limit,
                        Price::from_double(150.0), 10);
    engine.submit_order("AAPL", Side::Buy, OrderType::Limit,
                        Price::from_double(150.0), 4);
    EXPECT_EQ(engine.live_order_count(), 1u);   // taker filled, maker rests

    engine.submit_order("AAPL", Side::Buy, OrderType::Market, Price{0}, 6);
    EXPECT_EQ(engine.live_order_count(), 0u);

    engine.submit_order("AAPL", Side::Buy, OrderType::Limit, Price{0}, 1);
    engine.submit_order("AAPL", Side::Buy, OrderType::Limit,
                        Price::from_double(1.0), 1, TimeInForce::IOC);
    EXPECT_EQ(engine.live_order_count(), 0u);   // reject + IOC cancel
}

TEST_F(MatchingEngineTest, ModifyThroughEngine) {
    auto id = engine.submit_order("AAPL", Side::Buy, OrderType::Limit,
                                  Price::from_double(150.0), 10);
    EXPECT_TRUE(engine.modify_order(id, Price::from_double(151.0), 10));
    EXPECT_EQ(engine.get_book("AAPL")->best_bid()->to_double(), 151.0);
    EXPECT_FALSE(engine.modify_order(9999, Price::from_double(1.0), 1));
}

TEST_F(MatchingEngineTest, SelfTradePreventedByDefault) {
    engine.submit_order("AAPL", Side::Sell, OrderType::Limit,
                        Price::from_double(150.0), 10, TimeInForce::GTC, 42);
    listener.clear();
    engine.submit_order("AAPL", Side::Buy, OrderType::Limit,
                        Price::from_double(150.0), 10, TimeInForce::GTC, 42);

    EXPECT_TRUE(listener.trades.empty());
    EXPECT_EQ(listener.canceled.size(), 1u);
}

TEST_F(MatchingEngineTest, StopOrderLifecycleThroughEngine) {
    engine.submit_order("AAPL", Side::Buy, OrderType::Limit,
                        Price::from_double(95.0), 10);
    auto stop = engine.submit_stop_order("AAPL", Side::Sell, OrderType::Market,
                                         Price::from_double(95.0), Price{0}, 5);
    ASSERT_NE(stop, 0u);
    EXPECT_EQ(engine.live_order_count(), 2u);

    auto other = engine.submit_stop_order("AAPL", Side::Sell, OrderType::Market,
                                          Price::from_double(90.0), Price{0}, 5);
    EXPECT_TRUE(engine.cancel_order(other));

    engine.submit_order("AAPL", Side::Sell, OrderType::Limit,
                        Price::from_double(95.0), 1);   // trade at 95 -> trigger
    EXPECT_EQ(engine.live_order_count(), 1u);           // 4 left on the 95 bid
    EXPECT_EQ(engine.get_book("AAPL")->find(stop), nullptr);
}

TEST(MatchingEngineListener, DepthFeedIsOptIn) {
    struct DepthCounter : OrderListener {
        int n = 0;
        void on_depth(std::string_view, Side, const LevelInfo&) override { ++n; }
    } listener;

    MatchingEngine engine;
    engine.set_listener(&listener);
    engine.submit_order("AAPL", Side::Buy, OrderType::Limit, Price::from_double(1.0), 1);
    EXPECT_EQ(listener.n, 0);

    engine.set_depth_feed(true);
    engine.submit_order("AAPL", Side::Buy, OrderType::Limit, Price::from_double(1.0), 1);
    engine.submit_order("MSFT", Side::Buy, OrderType::Limit, Price::from_double(1.0), 1);
    EXPECT_EQ(listener.n, 2);
}

TEST(MatchingEngineListener, ListenerSetAfterBookCreation) {
    MatchingEngine engine;
    engine.submit_order("AAPL", Side::Sell, OrderType::Limit,
                        Price::from_double(150.0), 10);

    EngineListener listener;
    engine.set_listener(&listener);
    engine.submit_order("AAPL", Side::Buy, OrderType::Limit,
                        Price::from_double(150.0), 10);

    EXPECT_EQ(listener.trades.size(), 1u);
}
