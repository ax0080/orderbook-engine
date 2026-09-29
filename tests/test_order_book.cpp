#include <gtest/gtest.h>

#include "exchange/matching_engine.h"
#include "exchange/memory_pool.h"
#include "exchange/order.h"
#include "exchange/order_book.h"
#include "exchange/order_listener.h"
#include "exchange/types.h"

#include <string>
#include <vector>

using namespace exchange;

// ---------------------------------------------------------------------------
// Test listener that records every callback
// ---------------------------------------------------------------------------
struct TestListener : OrderListener {
    struct FillRecord {
        OrderId  id;
        Price    price;
        Quantity qty;
        Quantity remaining;
    };

    std::vector<OrderId>     accepted;
    std::vector<Trade>       trades;
    std::vector<FillRecord>  fills;
    std::vector<OrderId>     canceled;
    std::vector<std::string> rejections;
    std::vector<OrderId>     modified;
    std::vector<std::string> modify_rejections;
    std::vector<BookTop>     bbos;
    std::vector<OrderId>     triggered;
    std::vector<std::pair<Side, LevelInfo>> depth_updates;
    std::vector<std::string> log;   // event order, e.g. "trade", "fill:3"

    void on_stop_trigger(OrderId id) override {
        triggered.push_back(id);
        log.push_back("trigger:" + std::to_string(id));
    }

    void on_depth(std::string_view, Side side, const LevelInfo& l) override {
        depth_updates.emplace_back(side, l);
    }

    void on_accept(OrderId id) override {
        accepted.push_back(id);
        log.push_back("accept:" + std::to_string(id));
    }

    void on_fill(OrderId id, Price p, Quantity q, Quantity r) override {
        fills.push_back({id, p, q, r});
        log.push_back("fill:" + std::to_string(id));
    }

    void on_cancel(OrderId id, Quantity) override {
        canceled.push_back(id);
        log.push_back("cancel:" + std::to_string(id));
    }

    void on_reject(OrderId, const char* reason) override {
        rejections.emplace_back(reason);
    }

    void on_modify(OrderId id, Price, Quantity) override { modified.push_back(id); }

    void on_modify_reject(OrderId, const char* reason) override {
        modify_rejections.emplace_back(reason);
    }

    void on_trade(const Trade& t) override {
        trades.push_back(t);
        log.push_back("trade");
    }

    void on_bbo(std::string_view, const BookTop& top) override { bbos.push_back(top); }

    void clear() {
        accepted.clear();
        trades.clear();
        fills.clear();
        canceled.clear();
        rejections.clear();
        modified.clear();
        modify_rejections.clear();
        bbos.clear();
        triggered.clear();
        depth_updates.clear();
        log.clear();
    }
};

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------
class OrderBookTest : public ::testing::Test {
protected:
    MemoryPool<Order> pool;
    TestListener      listener;
    OrderBook         book{"AAPL", pool, &listener};
    OrderId           next_id = 1;
    Timestamp         ts      = 1000;

    void SetUp() override { book.set_depth_feed(true); }

    OrderId buy_limit(double price, Quantity qty) {
        OrderId id = next_id++;
        book.submit(id, Side::Buy, OrderType::Limit,
                    Price::from_double(price), qty, ts++);
        return id;
    }

    OrderId sell_limit(double price, Quantity qty) {
        OrderId id = next_id++;
        book.submit(id, Side::Sell, OrderType::Limit,
                    Price::from_double(price), qty, ts++);
        return id;
    }

    OrderId buy_market(Quantity qty) {
        OrderId id = next_id++;
        book.submit(id, Side::Buy, OrderType::Market,
                    Price{0}, qty, ts++);
        return id;
    }

    OrderId sell_market(Quantity qty) {
        OrderId id = next_id++;
        book.submit(id, Side::Sell, OrderType::Market,
                    Price{0}, qty, ts++);
        return id;
    }

    OrderId submit(OrderBook& b, Side side, double price, Quantity qty,
                   TimeInForce tif = TimeInForce::GTC, TraderId trader = 0) {
        OrderId id = next_id++;
        b.submit(id, side, OrderType::Limit, Price::from_double(price), qty,
                 ts++, tif, trader);
        return id;
    }

    OrderId stop_market(Side side, double stop, Quantity qty) {
        OrderId id = next_id++;
        book.submit_stop(id, side, OrderType::Market, Price::from_double(stop),
                         Price{0}, qty);
        return id;
    }

    OrderId stop_limit(Side side, double stop, double limit, Quantity qty) {
        OrderId id = next_id++;
        book.submit_stop(id, side, OrderType::Limit, Price::from_double(stop),
                         Price::from_double(limit), qty);
        return id;
    }
};

// ---------------------------------------------------------------------------
// Basic tests
// ---------------------------------------------------------------------------

TEST_F(OrderBookTest, EmptyBook) {
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.order_count(), 0u);
}

TEST_F(OrderBookTest, SingleBuyRests) {
    buy_limit(100.0, 10);
    ASSERT_TRUE(book.best_bid().has_value());
    EXPECT_EQ(book.best_bid()->to_double(), 100.0);
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.order_count(), 1u);
}

TEST_F(OrderBookTest, SingleSellRests) {
    sell_limit(105.0, 5);
    ASSERT_TRUE(book.best_ask().has_value());
    EXPECT_EQ(book.best_ask()->to_double(), 105.0);
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_EQ(book.order_count(), 1u);
}

TEST_F(OrderBookTest, BidAskSpread) {
    buy_limit(99.0, 10);
    sell_limit(101.0, 10);
    EXPECT_EQ(book.best_bid()->to_double(), 99.0);
    EXPECT_EQ(book.best_ask()->to_double(), 101.0);
    EXPECT_EQ(book.order_count(), 2u);
}

// ---------------------------------------------------------------------------
// Matching tests
// ---------------------------------------------------------------------------

TEST_F(OrderBookTest, ExactMatch) {
    sell_limit(100.0, 10);
    listener.clear();

    buy_limit(100.0, 10);

    EXPECT_EQ(listener.trades.size(), 1u);
    EXPECT_EQ(listener.trades[0].quantity, 10u);
    EXPECT_EQ(listener.trades[0].price.to_double(), 100.0);
    EXPECT_EQ(book.order_count(), 0u);
}

TEST_F(OrderBookTest, PartialFillAggressor) {
    sell_limit(100.0, 5);
    listener.clear();

    buy_limit(100.0, 10);

    EXPECT_EQ(listener.trades.size(), 1u);
    EXPECT_EQ(listener.trades[0].quantity, 5u);

    // Remaining 5 of buyer rests on book
    EXPECT_EQ(book.order_count(), 1u);
    ASSERT_TRUE(book.best_bid().has_value());
    EXPECT_EQ(book.best_bid()->to_double(), 100.0);
}

TEST_F(OrderBookTest, PartialFillPassive) {
    sell_limit(100.0, 20);
    listener.clear();

    buy_limit(100.0, 5);

    EXPECT_EQ(listener.trades.size(), 1u);
    EXPECT_EQ(listener.trades[0].quantity, 5u);

    // Remaining 15 of seller stays
    EXPECT_EQ(book.order_count(), 1u);
    ASSERT_TRUE(book.best_ask().has_value());
}

TEST_F(OrderBookTest, MultipleLevelsSwept) {
    sell_limit(100.0, 3);
    sell_limit(101.0, 5);
    sell_limit(102.0, 7);
    listener.clear();

    buy_limit(101.5, 10);

    // Should sweep 100.0 (3) + 101.0 (5) = 8, remaining 2 rests on bid
    EXPECT_EQ(listener.trades.size(), 2u);
    EXPECT_EQ(listener.trades[0].quantity, 3u);
    EXPECT_EQ(listener.trades[0].price.to_double(), 100.0);
    EXPECT_EQ(listener.trades[1].quantity, 5u);
    EXPECT_EQ(listener.trades[1].price.to_double(), 101.0);

    // 2 remaining from buyer rests, 102.0 sell still there
    EXPECT_EQ(book.order_count(), 2u);
    EXPECT_EQ(book.best_bid()->to_double(), 101.5);
    EXPECT_EQ(book.best_ask()->to_double(), 102.0);
}

TEST_F(OrderBookTest, FIFOWithinPriceLevel) {
    auto id1 = sell_limit(100.0, 5);
    auto id2 = sell_limit(100.0, 5);
    listener.clear();

    buy_limit(100.0, 3);

    ASSERT_EQ(listener.trades.size(), 1u);
    EXPECT_EQ(listener.trades[0].seller_order_id, id1);
    EXPECT_EQ(listener.trades[0].quantity, 3u);

    // id1 should be partially filled (2 remaining), id2 untouched
    EXPECT_EQ(book.order_count(), 2u);

    listener.clear();
    buy_limit(100.0, 4);

    // Should take remaining 2 from id1, then 2 from id2
    ASSERT_EQ(listener.trades.size(), 2u);
    EXPECT_EQ(listener.trades[0].seller_order_id, id1);
    EXPECT_EQ(listener.trades[0].quantity, 2u);
    EXPECT_EQ(listener.trades[1].seller_order_id, id2);
    EXPECT_EQ(listener.trades[1].quantity, 2u);
}

TEST_F(OrderBookTest, SellAggressorMatchesBids) {
    buy_limit(100.0, 10);
    buy_limit(99.0, 5);
    listener.clear();

    sell_limit(99.5, 8);

    // Should match against 100.0 bid (highest bid >= 99.5)
    ASSERT_EQ(listener.trades.size(), 1u);
    EXPECT_EQ(listener.trades[0].price.to_double(), 100.0);
    EXPECT_EQ(listener.trades[0].quantity, 8u);

    EXPECT_EQ(book.order_count(), 2u); // 2 remaining on 100.0 + 5 on 99.0
}

// ---------------------------------------------------------------------------
// Cancel tests
// ---------------------------------------------------------------------------

TEST_F(OrderBookTest, CancelExistingOrder) {
    auto id = buy_limit(100.0, 10);
    EXPECT_TRUE(book.cancel(id));
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_FALSE(book.best_bid().has_value());
}

TEST_F(OrderBookTest, CancelNonExistentOrder) {
    EXPECT_FALSE(book.cancel(999));
}

TEST_F(OrderBookTest, CancelCleansEmptyLevel) {
    auto id1 = buy_limit(100.0, 5);
    buy_limit(99.0, 5);

    book.cancel(id1);
    EXPECT_EQ(book.bid_level_count(), 1u);
    EXPECT_EQ(book.best_bid()->to_double(), 99.0);
}

TEST_F(OrderBookTest, CancelMiddleOfLevel) {
    auto id1 = sell_limit(100.0, 1);
    auto id2 = sell_limit(100.0, 2);
    auto id3 = sell_limit(100.0, 3);

    book.cancel(id2);
    EXPECT_EQ(book.order_count(), 2u);
    EXPECT_EQ(book.ask_level_count(), 1u);

    // id1 and id3 should still match in FIFO order
    listener.clear();
    buy_limit(100.0, 4);
    ASSERT_EQ(listener.trades.size(), 2u);
    EXPECT_EQ(listener.trades[0].seller_order_id, id1);
    EXPECT_EQ(listener.trades[0].quantity, 1u);
    EXPECT_EQ(listener.trades[1].seller_order_id, id3);
    EXPECT_EQ(listener.trades[1].quantity, 3u);
}

// ---------------------------------------------------------------------------
// Market orders
// ---------------------------------------------------------------------------

TEST_F(OrderBookTest, MarketBuyFullFill) {
    sell_limit(100.0, 10);
    listener.clear();

    buy_market(10);

    ASSERT_EQ(listener.trades.size(), 1u);
    EXPECT_EQ(listener.trades[0].quantity, 10u);
    EXPECT_EQ(book.order_count(), 0u);
}

TEST_F(OrderBookTest, MarketBuyPartialCancelsRemainder) {
    sell_limit(100.0, 3);
    listener.clear();

    buy_market(10);

    EXPECT_EQ(listener.trades.size(), 1u);
    EXPECT_EQ(listener.trades[0].quantity, 3u);
    EXPECT_FALSE(listener.canceled.empty());
    EXPECT_EQ(book.order_count(), 0u);
}

TEST_F(OrderBookTest, MarketSellSweepsMultipleBidLevels) {
    buy_limit(101.0, 5);
    buy_limit(100.0, 5);
    listener.clear();

    sell_market(8);

    ASSERT_EQ(listener.trades.size(), 2u);
    EXPECT_EQ(listener.trades[0].price.to_double(), 101.0);
    EXPECT_EQ(listener.trades[0].quantity, 5u);
    EXPECT_EQ(listener.trades[1].price.to_double(), 100.0);
    EXPECT_EQ(listener.trades[1].quantity, 3u);
}

// ---------------------------------------------------------------------------
// Rejection tests
// ---------------------------------------------------------------------------

TEST_F(OrderBookTest, RejectZeroQuantity) {
    book.submit(1, Side::Buy, OrderType::Limit,
                Price::from_double(100.0), 0, ts);
    EXPECT_FALSE(listener.rejections.empty());
}

TEST_F(OrderBookTest, RejectInvalidPrice) {
    book.submit(1, Side::Buy, OrderType::Limit,
                Price{0}, 10, ts);
    EXPECT_FALSE(listener.rejections.empty());
}

TEST_F(OrderBookTest, RejectDuplicateId) {
    book.submit(1, Side::Buy, OrderType::Limit,
                Price::from_double(100.0), 10, ts);
    listener.clear();
    book.submit(1, Side::Buy, OrderType::Limit,
                Price::from_double(100.0), 10, ts);
    EXPECT_FALSE(listener.rejections.empty());
}

// ---------------------------------------------------------------------------
// Fixed-point price precision
// ---------------------------------------------------------------------------

TEST_F(OrderBookTest, FixedPointPrecision) {
    Price p = Price::from_double(123.4567);
    EXPECT_EQ(p.raw, 1234567);
    EXPECT_NEAR(p.to_double(), 123.4567, 1e-9);
}

TEST_F(OrderBookTest, PriceComparisonCorrectness) {
    Price a = Price::from_double(100.00);
    Price b = Price::from_double(100.01);
    EXPECT_TRUE(a < b);
    EXPECT_TRUE(b > a);
    EXPECT_TRUE(a != b);
    EXPECT_FALSE(a == b);
}

// ---------------------------------------------------------------------------
// Time in force
// ---------------------------------------------------------------------------

TEST_F(OrderBookTest, IocPartialFillCancelsRemainder) {
    sell_limit(100.0, 3);
    listener.clear();

    auto id = submit(book, Side::Buy, 100.0, 10, TimeInForce::IOC);

    ASSERT_EQ(listener.trades.size(), 1u);
    EXPECT_EQ(listener.trades[0].quantity, 3u);
    ASSERT_EQ(listener.canceled.size(), 1u);
    EXPECT_EQ(listener.canceled[0], id);
    EXPECT_EQ(book.order_count(), 0u);
}

TEST_F(OrderBookTest, IocWithoutLiquidityNeverRests) {
    submit(book, Side::Buy, 100.0, 10, TimeInForce::IOC);
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_EQ(listener.canceled.size(), 1u);
}

TEST_F(OrderBookTest, FokInsufficientLiquidityCancelsWithoutTrading) {
    sell_limit(100.0, 5);
    sell_limit(101.0, 4);
    listener.clear();

    submit(book, Side::Buy, 101.0, 10, TimeInForce::FOK);

    EXPECT_TRUE(listener.trades.empty());
    EXPECT_EQ(listener.canceled.size(), 1u);
    EXPECT_EQ(book.order_count(), 2u);
}

TEST_F(OrderBookTest, FokFillsAcrossLevels) {
    sell_limit(100.0, 5);
    sell_limit(101.0, 5);
    listener.clear();

    submit(book, Side::Buy, 101.0, 10, TimeInForce::FOK);

    EXPECT_EQ(listener.trades.size(), 2u);
    EXPECT_TRUE(listener.canceled.empty());
    EXPECT_EQ(book.order_count(), 0u);
}

TEST_F(OrderBookTest, FokIgnoresLiquidityBeyondLimitPrice) {
    sell_limit(100.0, 5);
    sell_limit(102.0, 5);
    listener.clear();

    submit(book, Side::Buy, 101.0, 10, TimeInForce::FOK);
    EXPECT_TRUE(listener.trades.empty());
}

// ---------------------------------------------------------------------------
// Self-trade prevention
// ---------------------------------------------------------------------------

TEST_F(OrderBookTest, StpCancelTakerKeepsRestingOrder) {
    OrderBook b{"X", pool, &listener, StpPolicy::CancelTaker};
    auto resting = submit(b, Side::Sell, 100.0, 5, TimeInForce::GTC, 7);
    listener.clear();

    auto taker = submit(b, Side::Buy, 100.0, 5, TimeInForce::GTC, 7);

    EXPECT_TRUE(listener.trades.empty());
    ASSERT_EQ(listener.canceled.size(), 1u);
    EXPECT_EQ(listener.canceled[0], taker);
    EXPECT_TRUE(b.has_order(resting));
}

TEST_F(OrderBookTest, StpCancelRestingThenMatchesOthers) {
    OrderBook b{"X", pool, &listener, StpPolicy::CancelResting};
    auto own   = submit(b, Side::Sell, 100.0, 5, TimeInForce::GTC, 7);
    auto other = submit(b, Side::Sell, 100.0, 5, TimeInForce::GTC, 8);
    listener.clear();

    submit(b, Side::Buy, 100.0, 5, TimeInForce::GTC, 7);

    ASSERT_EQ(listener.canceled.size(), 1u);
    EXPECT_EQ(listener.canceled[0], own);
    ASSERT_EQ(listener.trades.size(), 1u);
    EXPECT_EQ(listener.trades[0].seller_order_id, other);
    EXPECT_EQ(b.order_count(), 0u);
}

TEST_F(OrderBookTest, StpCancelBoth) {
    OrderBook b{"X", pool, &listener, StpPolicy::CancelBoth};
    submit(b, Side::Sell, 100.0, 5, TimeInForce::GTC, 7);
    listener.clear();

    submit(b, Side::Buy, 100.0, 5, TimeInForce::GTC, 7);

    EXPECT_TRUE(listener.trades.empty());
    EXPECT_EQ(listener.canceled.size(), 2u);
    EXPECT_EQ(b.order_count(), 0u);
}

TEST_F(OrderBookTest, StpNoneAllowsSelfTrade) {
    OrderBook b{"X", pool, &listener, StpPolicy::None};
    submit(b, Side::Sell, 100.0, 5, TimeInForce::GTC, 7);
    listener.clear();

    submit(b, Side::Buy, 100.0, 5, TimeInForce::GTC, 7);
    EXPECT_EQ(listener.trades.size(), 1u);
}

TEST_F(OrderBookTest, StpIgnoresAnonymousTrader) {
    submit(book, Side::Sell, 100.0, 5);
    listener.clear();

    submit(book, Side::Buy, 100.0, 5);
    EXPECT_EQ(listener.trades.size(), 1u);
}

TEST_F(OrderBookTest, FokCountsOnlyNonSelfLiquidityUnderCancelResting) {
    OrderBook b{"X", pool, &listener, StpPolicy::CancelResting};
    submit(b, Side::Sell, 100.0, 5, TimeInForce::GTC, 7);
    submit(b, Side::Sell, 100.0, 5, TimeInForce::GTC, 8);
    listener.clear();

    submit(b, Side::Buy, 100.0, 10, TimeInForce::FOK, 7);

    EXPECT_TRUE(listener.trades.empty());
    EXPECT_EQ(b.order_count(), 2u);
}

// ---------------------------------------------------------------------------
// Modify
// ---------------------------------------------------------------------------

TEST_F(OrderBookTest, ModifyReduceQtyKeepsPriority) {
    auto first  = sell_limit(100.0, 10);
    auto second = sell_limit(100.0, 10);

    EXPECT_TRUE(book.modify(first, Price::from_double(100.0), 4, ts++));
    EXPECT_EQ(book.top().ask_qty, 14u);
    listener.clear();

    buy_limit(100.0, 4);
    ASSERT_EQ(listener.trades.size(), 1u);
    EXPECT_EQ(listener.trades[0].seller_order_id, first);
    EXPECT_TRUE(book.has_order(second));
}

TEST_F(OrderBookTest, ModifyIncreaseQtyLosesPriority) {
    auto first  = sell_limit(100.0, 5);
    auto second = sell_limit(100.0, 5);

    book.modify(first, Price::from_double(100.0), 8, ts++);
    listener.clear();

    buy_limit(100.0, 5);
    ASSERT_EQ(listener.trades.size(), 1u);
    EXPECT_EQ(listener.trades[0].seller_order_id, second);
    EXPECT_EQ(book.find(first)->remaining(), 8u);
}

TEST_F(OrderBookTest, ModifyPriceMovesLevel) {
    auto id = buy_limit(99.0, 5);
    book.modify(id, Price::from_double(98.0), 5, ts++);

    EXPECT_EQ(book.best_bid()->to_double(), 98.0);
    EXPECT_EQ(book.bid_level_count(), 1u);
}

TEST_F(OrderBookTest, ModifyPriceThroughSpreadMatches) {
    sell_limit(101.0, 3);
    auto id = buy_limit(99.0, 5);
    listener.clear();

    book.modify(id, Price::from_double(101.0), 5, ts++);

    ASSERT_EQ(listener.trades.size(), 1u);
    EXPECT_EQ(listener.trades[0].buyer_order_id, id);
    EXPECT_EQ(listener.trades[0].quantity, 3u);
    EXPECT_EQ(book.find(id)->remaining(), 2u);
    EXPECT_EQ(book.best_bid()->to_double(), 101.0);
}

TEST_F(OrderBookTest, ModifyPartiallyFilledUsesTotalQuantity) {
    auto id = sell_limit(100.0, 10);
    buy_limit(100.0, 4);   // 6 remaining

    EXPECT_TRUE(book.modify(id, Price::from_double(100.0), 7, ts++));
    EXPECT_EQ(book.find(id)->remaining(), 3u);

    EXPECT_FALSE(book.modify(id, Price::from_double(100.0), 4, ts++));
    EXPECT_EQ(listener.modify_rejections.back(), "quantity at or below filled");
    EXPECT_TRUE(book.has_order(id));
}

TEST_F(OrderBookTest, ModifyUnknownOrderRejected) {
    EXPECT_FALSE(book.modify(42, Price::from_double(100.0), 5, ts++));
    ASSERT_EQ(listener.modify_rejections.size(), 1u);
}

// ---------------------------------------------------------------------------
// Market data
// ---------------------------------------------------------------------------

TEST_F(OrderBookTest, BboEmittedOnTopChange) {
    buy_limit(99.0, 5);
    ASSERT_EQ(listener.bbos.size(), 1u);
    EXPECT_EQ(listener.bbos[0].bid_price.to_double(), 99.0);
    EXPECT_EQ(listener.bbos[0].bid_qty, 5u);
    EXPECT_EQ(listener.bbos[0].ask_qty, 0u);

    buy_limit(99.0, 3);
    ASSERT_EQ(listener.bbos.size(), 2u);
    EXPECT_EQ(listener.bbos[1].bid_qty, 8u);
}

TEST_F(OrderBookTest, BboNotEmittedBehindTop) {
    buy_limit(99.0, 5);
    listener.clear();

    buy_limit(98.0, 5);
    sell_limit(105.0, 1);
    sell_limit(106.0, 1);
    EXPECT_EQ(listener.bbos.size(), 1u);   // only the first ask moves the top
}

TEST_F(OrderBookTest, DepthAggregatesLevels) {
    buy_limit(99.0, 5);
    buy_limit(99.0, 7);
    buy_limit(98.0, 1);
    buy_limit(97.0, 1);

    auto levels = book.depth(Side::Buy, 2);
    ASSERT_EQ(levels.size(), 2u);
    EXPECT_EQ(levels[0].price.to_double(), 99.0);
    EXPECT_EQ(levels[0].quantity, 12u);
    EXPECT_EQ(levels[0].order_count, 2u);
    EXPECT_EQ(levels[1].price.to_double(), 98.0);
}

TEST_F(OrderBookTest, DepthFeedReportsChangedLevels) {
    buy_limit(99.0, 5);
    ASSERT_EQ(listener.depth_updates.size(), 1u);
    EXPECT_EQ(listener.depth_updates[0].first, Side::Buy);
    EXPECT_EQ(listener.depth_updates[0].second.quantity, 5u);

    sell_limit(101.0, 3);
    sell_limit(102.0, 3);
    listener.clear();

    buy_limit(102.0, 6);   // sweeps both ask levels

    ASSERT_EQ(listener.depth_updates.size(), 2u);
    for (const auto& [side, level] : listener.depth_updates) {
        EXPECT_EQ(side, Side::Sell);
        EXPECT_EQ(level.quantity, 0u);   // level removed
        EXPECT_EQ(level.order_count, 0u);
    }
}

TEST_F(OrderBookTest, DepthFeedCoalescesPerOperation) {
    sell_limit(101.0, 3);
    sell_limit(101.0, 3);
    sell_limit(101.0, 3);
    listener.clear();

    buy_limit(101.0, 7);   // three fills on one level -> one update

    ASSERT_EQ(listener.depth_updates.size(), 1u);
    EXPECT_EQ(listener.depth_updates[0].second.quantity, 2u);
    EXPECT_EQ(listener.depth_updates[0].second.order_count, 1u);
}

// ---------------------------------------------------------------------------
// Stop orders
// ---------------------------------------------------------------------------

TEST_F(OrderBookTest, StopIsHeldOffBook) {
    buy_limit(99.0, 5);
    listener.clear();

    stop_market(Side::Sell, 95.0, 10);

    EXPECT_EQ(book.order_count(), 1u);
    EXPECT_EQ(book.stop_order_count(), 1u);
    EXPECT_EQ(listener.accepted.size(), 1u);
    EXPECT_TRUE(listener.bbos.empty());
    EXPECT_TRUE(listener.depth_updates.empty());
}

TEST_F(OrderBookTest, SellStopTriggersWhenTradeAtStopPrice) {
    buy_limit(95.0, 10);
    buy_limit(94.0, 10);
    auto stop = stop_market(Side::Sell, 95.0, 5);
    listener.clear();

    sell_limit(95.0, 10);   // trades at 95 -> triggers stop -> sells into 94

    ASSERT_EQ(listener.triggered.size(), 1u);
    EXPECT_EQ(listener.triggered[0], stop);
    ASSERT_EQ(listener.trades.size(), 2u);
    EXPECT_EQ(listener.trades[1].seller_order_id, stop);
    EXPECT_EQ(listener.trades[1].price.to_double(), 94.0);
    EXPECT_EQ(book.stop_order_count(), 0u);
}

TEST_F(OrderBookTest, BuyStopLimitRestsAfterTrigger) {
    sell_limit(101.0, 5);
    auto stop = stop_limit(Side::Buy, 101.0, 101.5, 10);
    listener.clear();

    buy_limit(101.0, 5);   // trade at 101 -> buy stop-limit 10 @ 101.5, no asks left

    ASSERT_EQ(listener.triggered.size(), 1u);
    ASSERT_NE(book.find(stop), nullptr);
    EXPECT_EQ(book.best_bid()->to_double(), 101.5);
}

TEST_F(OrderBookTest, StopCascade) {
    buy_limit(95.0, 1);
    buy_limit(94.0, 1);
    buy_limit(93.0, 1);
    auto s1 = stop_market(Side::Sell, 95.0, 1);
    auto s2 = stop_market(Side::Sell, 94.0, 1);
    listener.clear();

    sell_limit(95.0, 1);   // 95 -> s1 hits 94 -> s2 hits 93

    std::vector<OrderId> expected = {s1, s2};
    EXPECT_EQ(listener.triggered, expected);
    ASSERT_EQ(listener.trades.size(), 3u);
    EXPECT_EQ(listener.trades[2].price.to_double(), 93.0);
    EXPECT_EQ(book.last_trade_price()->to_double(), 93.0);
    EXPECT_EQ(book.order_count(), 0u);
}

TEST_F(OrderBookTest, StopsTriggerInPriceThenTimeOrder) {
    for (int i = 0; i < 3; ++i) buy_limit(90.0, 1);
    auto late_high  = stop_market(Side::Sell, 96.0, 1);
    auto early_low  = stop_market(Side::Sell, 95.0, 1);
    auto late_low   = stop_market(Side::Sell, 95.0, 1);
    listener.clear();

    sell_limit(90.0, 1);

    std::vector<OrderId> expected = {late_high, early_low, late_low};
    EXPECT_EQ(listener.triggered, expected);
}

TEST_F(OrderBookTest, StopRejectedIfAlreadyTriggerable) {
    sell_limit(100.0, 1);
    buy_limit(100.0, 1);   // last = 100
    listener.clear();

    stop_market(Side::Sell, 101.0, 1);
    stop_market(Side::Buy, 99.0, 1);

    EXPECT_EQ(listener.rejections.size(), 2u);
    EXPECT_EQ(book.stop_order_count(), 0u);
}

TEST_F(OrderBookTest, CancelStopOrder) {
    auto id = stop_market(Side::Sell, 95.0, 10);
    listener.clear();

    EXPECT_TRUE(book.cancel(id));
    EXPECT_EQ(book.stop_order_count(), 0u);
    ASSERT_EQ(listener.canceled.size(), 1u);
    EXPECT_FALSE(book.cancel(id));
}

TEST_F(OrderBookTest, ModifyStopOrderRejected) {
    auto id = stop_market(Side::Sell, 95.0, 10);
    EXPECT_FALSE(book.modify(id, Price::from_double(94.0), 10, ts++));
    EXPECT_EQ(listener.modify_rejections.back(), "stop orders cannot be modified");
}

// ---------------------------------------------------------------------------
// Event delivery
// ---------------------------------------------------------------------------

TEST_F(OrderBookTest, EventOrderTradeThenFills) {
    auto maker = sell_limit(100.0, 5);
    listener.clear();
    auto taker = buy_limit(100.0, 5);

    std::vector<std::string> expected = {
        "accept:" + std::to_string(taker), "trade",
        "fill:" + std::to_string(taker), "fill:" + std::to_string(maker)};
    EXPECT_EQ(listener.log, expected);
}

struct ReentrantListener : OrderListener {
    OrderBook*               book = nullptr;
    std::vector<std::string> log;

    void on_accept(OrderId id) override { log.push_back("accept:" + std::to_string(id)); }
    void on_trade(const Trade&) override {
        log.push_back("trade");
        if (!book->has_order(100))
            book->submit(100, Side::Sell, OrderType::Limit,
                         Price::from_double(101.0), 7, 99);
    }
};

TEST(OrderBookReentrancy, ListenerMaySubmitFromCallback) {
    MemoryPool<Order> pool;
    ReentrantListener listener;
    OrderBook         book{"X", pool, &listener};
    listener.book = &book;

    book.submit(1, Side::Sell, OrderType::Limit, Price::from_double(100.0), 5, 1);
    book.submit(2, Side::Buy,  OrderType::Limit, Price::from_double(100.0), 5, 2);

    std::vector<std::string> expected = {"accept:1", "accept:2", "trade", "accept:100"};
    EXPECT_EQ(listener.log, expected);
    EXPECT_EQ(book.best_ask()->to_double(), 101.0);
}
