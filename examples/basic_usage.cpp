#include "exchange/matching_engine.h"

#include <cstdio>

using namespace exchange;

struct PrintListener : OrderListener {
    void on_trade(const Trade& t) override {
        std::printf("  TRADE  %.*s  %u @ %.2f  (buy #%llu / sell #%llu)\n",
                    static_cast<int>(t.symbol.size()), t.symbol.data(),
                    t.quantity, t.price.to_double(),
                    static_cast<unsigned long long>(t.buyer_order_id),
                    static_cast<unsigned long long>(t.seller_order_id));
    }
    void on_cancel(OrderId id, Quantity remaining) override {
        std::printf("  CANCEL #%llu  (%u unfilled)\n",
                    static_cast<unsigned long long>(id), remaining);
    }
    void on_reject(OrderId id, const char* reason) override {
        std::printf("  REJECT #%llu  %s\n", static_cast<unsigned long long>(id), reason);
    }
    void on_stop_trigger(OrderId id) override {
        std::printf("  STOP   #%llu triggered\n", static_cast<unsigned long long>(id));
    }
    void on_bbo(std::string_view sym, const BookTop& top) override {
        std::printf("  BBO    %.*s  %llu @ %.2f | %.2f @ %llu\n",
                    static_cast<int>(sym.size()), sym.data(),
                    static_cast<unsigned long long>(top.bid_qty), top.bid_price.to_double(),
                    top.ask_price.to_double(), static_cast<unsigned long long>(top.ask_qty));
    }
};

static void print_book(const OrderBook& book) {
    auto asks = book.depth(Side::Sell, 5);
    auto bids = book.depth(Side::Buy, 5);
    std::printf("\n  %-8s %10s %6s\n", book.symbol().c_str(), "price", "qty");
    for (auto it = asks.rbegin(); it != asks.rend(); ++it)
        std::printf("  %-8s %10.2f %6llu\n", "ask", it->price.to_double(),
                    static_cast<unsigned long long>(it->quantity));
    for (const auto& l : bids)
        std::printf("  %-8s %10.2f %6llu\n", "bid", l.price.to_double(),
                    static_cast<unsigned long long>(l.quantity));
    std::printf("\n");
}

static Price px(double p) { return Price::from_double(p); }

int main() {
    PrintListener  listener;
    MatchingEngine engine;   // self-trade prevention: CancelTaker
    engine.set_listener(&listener);

    std::puts("== Build a book");
    engine.submit_order("AAPL", Side::Sell, OrderType::Limit, px(101.00), 300);
    engine.submit_order("AAPL", Side::Sell, OrderType::Limit, px(100.50), 200);
    engine.submit_order("AAPL", Side::Buy,  OrderType::Limit, px(99.50),  400);
    OrderId bid = engine.submit_order("AAPL", Side::Buy, OrderType::Limit, px(99.00), 100);
    print_book(*engine.get_book("AAPL"));

    std::puts("== Aggressive buy 350 @ 101.00 sweeps two levels");
    engine.submit_order("AAPL", Side::Buy, OrderType::Limit, px(101.00), 350);
    print_book(*engine.get_book("AAPL"));

    std::puts("== IOC sell 1000 @ 99.50: fills what it can, cancels the rest");
    engine.submit_order("AAPL", Side::Sell, OrderType::Limit, px(99.50), 1000, TimeInForce::IOC);

    std::puts("\n== FOK buy 500 @ 101.00: not enough liquidity, nothing trades");
    engine.submit_order("AAPL", Side::Buy, OrderType::Limit, px(101.00), 500, TimeInForce::FOK);

    std::puts("\n== Modify bid #4 to 99.80 (loses queue priority)");
    engine.modify_order(bid, px(99.80), 100);
    print_book(*engine.get_book("AAPL"));

    std::puts("== Trader 7 crosses its own order: self-trade prevented");
    engine.submit_order("AAPL", Side::Sell, OrderType::Limit, px(100.50), 50, TimeInForce::GTC, 7);
    engine.submit_order("AAPL", Side::Buy,  OrderType::Limit, px(100.50), 50, TimeInForce::GTC, 7);

    std::puts("\n== Stop cascade: two sell stops below the market");
    engine.submit_order("MSFT", Side::Buy, OrderType::Limit, px(50.00), 100);
    engine.submit_order("MSFT", Side::Buy, OrderType::Limit, px(49.00), 100);
    engine.submit_order("MSFT", Side::Buy, OrderType::Limit, px(48.00), 100);
    engine.submit_stop_order("MSFT", Side::Sell, OrderType::Market, px(50.00), Price{0}, 100);
    engine.submit_stop_order("MSFT", Side::Sell, OrderType::Market, px(49.00), Price{0}, 100);
    std::puts("  -- sell 100 @ 50.00 hits the bid, trade at 50 triggers the first stop,");
    std::puts("     its trade at 49 triggers the second");
    engine.submit_order("MSFT", Side::Sell, OrderType::Limit, px(50.00), 100);

    std::puts("\n== Invalid order");
    engine.submit_order("AAPL", Side::Buy, OrderType::Limit, px(100.00), 0);
}
