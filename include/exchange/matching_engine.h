#pragma once

#include "memory_pool.h"
#include "order.h"
#include "order_book.h"
#include "order_listener.h"
#include "types.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace exchange {

// Multi-symbol front end. Single-threaded by design: run one engine per
// core and shard symbols across engines.
//
// The engine sits between the books and the user's listener so it can keep
// the OrderId -> book routing table in sync with order lifecycles.
class MatchingEngine : private OrderListener {
public:
    explicit MatchingEngine(StpPolicy stp = StpPolicy::CancelTaker);

    void set_listener(OrderListener* listener) { listener_ = listener; }
    void set_depth_feed(bool enabled);   // applies to existing and future books

    // Returns the assigned order id, or 0 if the order was rejected.
    OrderId submit_order(const std::string& symbol, Side side, OrderType type,
                         Price price, Quantity qty,
                         TimeInForce tif = TimeInForce::GTC, TraderId trader = 0);
    // `type` is what the order becomes once triggered: Market, or Limit at limit_price.
    OrderId submit_stop_order(const std::string& symbol, Side side, OrderType type,
                              Price stop_price, Price limit_price, Quantity qty,
                              TimeInForce tif = TimeInForce::GTC, TraderId trader = 0);
    bool cancel_order(OrderId id);
    bool modify_order(OrderId id, Price new_price, Quantity new_qty);

    OrderBook*       get_book(const std::string& symbol);
    const OrderBook* get_book(const std::string& symbol) const;

    std::size_t live_order_count() const { return routes_.size(); }

private:
    OrderBook& get_or_create(const std::string& symbol);
    static Timestamp now();

    void on_accept(OrderId id) override;
    void on_reject(OrderId id, const char* reason) override;
    void on_fill(OrderId id, Price p, Quantity q, Quantity remaining) override;
    void on_cancel(OrderId id, Quantity remaining) override;
    void on_modify(OrderId id, Price p, Quantity q) override;
    void on_modify_reject(OrderId id, const char* reason) override;
    void on_stop_trigger(OrderId id) override;
    void on_trade(const Trade& t) override;
    void on_bbo(std::string_view symbol, const BookTop& top) override;
    void on_depth(std::string_view symbol, Side side, const LevelInfo& level) override;

    // pool_ must outlive books_: books return resting orders on destruction.
    MemoryPool<Order>                                           pool_;
    std::unordered_map<std::string, std::unique_ptr<OrderBook>> books_;
    std::unordered_map<OrderId, OrderBook*>                     routes_;
    OrderListener*                                              listener_ = nullptr;
    StpPolicy                                                   stp_;
    bool                                                        depth_feed_ = false;
    OrderId                                                     next_id_  = 1;
};

} // namespace exchange
