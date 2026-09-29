#include <gtest/gtest.h>

#include "exchange/memory_pool.h"
#include "exchange/order.h"
#include "exchange/order_book.h"
#include "exchange/order_command.h"
#include "exchange/spsc_queue.h"

#include <atomic>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

using namespace exchange;

namespace {

#if defined(__SANITIZE_THREAD__)
constexpr uint64_t kStressItems = 200'000;
constexpr int      kPipelineOps = 20'000;
#else
constexpr uint64_t kStressItems = 5'000'000;
constexpr int      kPipelineOps = 200'000;
#endif

} // namespace

TEST(SpscQueueTest, StartsEmpty) {
    SpscQueue<int, 8> q;
    int v = 0;
    EXPECT_TRUE(q.empty());
    EXPECT_FALSE(q.try_pop(v));
}

TEST(SpscQueueTest, Fifo) {
    SpscQueue<int, 8> q;
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(q.try_push(i));
    EXPECT_EQ(q.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        int v = -1;
        ASSERT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, i);
    }
    int v = -1;
    EXPECT_FALSE(q.try_pop(v));
    EXPECT_TRUE(q.empty());
}

// Slots come back to the producer in batches, or all at once when the
// consumer finds the queue empty.
TEST(SpscQueueTest, ReleasesSlotsInBatches) {
    using Q = SpscQueue<uint64_t, 64>;
    static_assert(Q::kReleaseBatch > 1);
    Q q;
    for (uint64_t i = 0; i < Q::capacity(); ++i) ASSERT_TRUE(q.try_push(i));

    uint64_t v = 0;
    for (std::size_t i = 0; i + 1 < Q::kReleaseBatch; ++i) ASSERT_TRUE(q.try_pop(v));
    EXPECT_FALSE(q.try_push(0));

    ASSERT_TRUE(q.try_pop(v));
    for (std::size_t i = 0; i < Q::kReleaseBatch; ++i) EXPECT_TRUE(q.try_push(i));
    EXPECT_FALSE(q.try_push(0));

    while (q.try_pop(v)) {}
    EXPECT_TRUE(q.empty());
}

TEST(SpscQueueTest, RejectsWhenFull) {
    SpscQueue<int, 4> q;
    for (int i = 0; i < 4; ++i) ASSERT_TRUE(q.try_push(i));
    EXPECT_FALSE(q.try_push(99));

    int v = -1;
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 0);
    EXPECT_TRUE(q.try_push(4));
    EXPECT_FALSE(q.try_push(5));
}

TEST(SpscQueueTest, WrapsAroundManyTimes) {
    SpscQueue<uint64_t, 4> q;
    uint64_t next_in = 0, next_out = 0;
    for (int round = 0; round < 10'000; ++round) {
        int n = 1 + round % 4;
        for (int i = 0; i < n; ++i) ASSERT_TRUE(q.try_push(next_in++));
        for (int i = 0; i < n; ++i) {
            uint64_t v = 0;
            ASSERT_TRUE(q.try_pop(v));
            ASSERT_EQ(v, next_out++);
        }
    }
    uint64_t v = 0;
    EXPECT_FALSE(q.try_pop(v));
    EXPECT_TRUE(q.empty());
}

// One producer, one consumer, a small queue so both sides constantly hit the
// full/empty paths. Every value must arrive exactly once and in order.
TEST(SpscQueueTest, TwoThreadsPreserveOrder) {
    SpscQueue<uint64_t, 64> q;

    std::thread producer([&] {
        for (uint64_t i = 0; i < kStressItems; ++i)
            while (!q.try_push(i)) std::this_thread::yield();
    });

    uint64_t expected = 0;
    bool     in_order = true;
    while (expected < kStressItems) {
        uint64_t v;
        if (!q.try_pop(v)) { std::this_thread::yield(); continue; }
        if (v != expected) { in_order = false; break; }
        ++expected;
    }
    producer.join();

    EXPECT_TRUE(in_order) << "out of order at " << expected;
    EXPECT_EQ(expected, kStressItems);
    uint64_t v = 0;
    EXPECT_FALSE(q.try_pop(v));
    EXPECT_TRUE(q.empty());
}

namespace {

struct TradeCounter : OrderListener {
    uint64_t trades = 0;
    uint64_t volume = 0;
    void on_trade(const Trade& t) override { ++trades; volume += t.quantity; }
};

std::vector<OrderCommand> make_flow(int n) {
    constexpr int64_t kMid = 1'000'000, kTick = 100;
    std::mt19937 rng(7);
    std::vector<OrderCommand> cmds;
    std::vector<OrderId>      live;
    OrderId id = 1;
    for (int i = 0; i < n; ++i) {
        OrderCommand c;
        uint32_t r = rng() % 100;
        if (r < 40 && !live.empty()) {
            std::size_t k = rng() % live.size();
            c.kind = OrderCommand::Kind::Cancel;
            c.id   = live[k];
            live[k] = live.back();
            live.pop_back();
        } else {
            c.kind = OrderCommand::Kind::Submit;
            c.id   = id++;
            c.side = (rng() & 1) ? Side::Buy : Side::Sell;
            int64_t off = static_cast<int64_t>(rng() % 20) - 5;   // some cross
            c.price = Price{c.side == Side::Buy ? kMid - off * kTick : kMid + off * kTick};
            c.qty   = 1 + rng() % 100;
            c.ts    = c.id;
            c.tif   = r >= 90 ? TimeInForce::IOC : TimeInForce::GTC;
            if (c.tif == TimeInForce::GTC) live.push_back(c.id);
        }
        cmds.push_back(c);
    }
    return cmds;
}

} // namespace

// The same command stream applied directly and through a gateway thread must
// leave identical books and identical trade totals.
TEST(SpscQueueTest, PipelineMatchesDirectExecution) {
    const auto cmds = make_flow(kPipelineOps);

    MemoryPool<Order> pool_a;
    TradeCounter      direct;
    OrderBook         book_a("X", pool_a, &direct);
    for (const auto& c : cmds) apply(book_a, c);

    MemoryPool<Order> pool_b;
    TradeCounter      piped;
    OrderBook         book_b("X", pool_b, &piped);
    SpscQueue<OrderCommand, 1024> q;

    std::thread gateway([&] {
        for (const auto& c : cmds)
            while (!q.try_push(c)) std::this_thread::yield();
    });

    std::size_t done = 0;
    while (done < cmds.size()) {
        OrderCommand c;
        if (!q.try_pop(c)) { std::this_thread::yield(); continue; }
        apply(book_b, c);
        ++done;
    }
    gateway.join();

    EXPECT_EQ(book_b.validate(), "");
    EXPECT_GT(direct.trades, 0u);
    EXPECT_EQ(piped.trades, direct.trades);
    EXPECT_EQ(piped.volume, direct.volume);
    EXPECT_EQ(book_b.order_count(), book_a.order_count());
    EXPECT_EQ(book_b.top(), book_a.top());

    for (Side s : {Side::Buy, Side::Sell}) {
        auto da = book_a.depth(s, 1000);
        auto db = book_b.depth(s, 1000);
        ASSERT_EQ(da.size(), db.size());
        for (std::size_t i = 0; i < da.size(); ++i) {
            EXPECT_EQ(db[i].price, da[i].price);
            EXPECT_EQ(db[i].quantity, da[i].quantity);
            EXPECT_EQ(db[i].order_count, da[i].order_count);
        }
    }
}
