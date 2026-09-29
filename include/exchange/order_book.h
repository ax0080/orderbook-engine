#pragma once

#include "memory_pool.h"
#include "order.h"
#include "order_listener.h"
#include "types.h"

#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace exchange {

struct PriceLevel {
    Price    price;
    Order*   head      = nullptr;
    Order*   tail      = nullptr;
    uint64_t total_qty = 0;   // sum of remaining quantity
    uint32_t count     = 0;

    void push_back(Order* o);
    void erase(Order* o);
    bool empty() const { return head == nullptr; }
};

class OrderBook {
public:
    OrderBook(std::string symbol, MemoryPool<Order>& pool,
              OrderListener* listener = nullptr,
              StpPolicy stp = StpPolicy::CancelTaker);
    ~OrderBook();

    OrderBook(const OrderBook&)            = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    bool submit(OrderId id, Side side, OrderType type, Price price,
                Quantity qty, Timestamp ts,
                TimeInForce tif = TimeInForce::GTC, TraderId trader = 0);
    // Held off-book until the last trade price reaches stop_price
    // (buy: last >= stop, sell: last <= stop), then submitted as `type`
    // (Market, or Limit at limit_price).
    bool submit_stop(OrderId id, Side side, OrderType type, Price stop_price,
                     Price limit_price, Quantity qty,
                     TimeInForce tif = TimeInForce::GTC, TraderId trader = 0);
    bool cancel(OrderId id);
    // new_qty is the new total quantity (including already filled).
    // Reducing quantity keeps time priority; changing price or increasing
    // quantity moves the order to the back of the queue and may re-match.
    bool modify(OrderId id, Price new_price, Quantity new_qty, Timestamp ts);

    // Off by default: per-level on_depth updates cost ~50% extra per operation.
    void set_depth_feed(bool enabled) { depth_feed_ = enabled; }

    bool         has_order(OrderId id) const { return index_.count(id) > 0; }
    const Order* find(OrderId id) const;

    std::optional<Price>   best_bid() const;
    std::optional<Price>   best_ask() const;
    BookTop                top() const;
    std::vector<LevelInfo> depth(Side side, std::size_t max_levels) const;

    std::size_t bid_level_count() const { return bids_.size(); }
    std::size_t ask_level_count() const { return asks_.size(); }
    std::size_t order_count()      const { return index_.size(); }
    std::size_t stop_order_count() const { return stop_index_.size(); }
    std::optional<Price> last_trade_price() const { return last_trade_; }

    const std::string& symbol() const { return symbol_; }

    // Full O(n) consistency check of levels, links, totals and indexes.
    // Returns an empty string if the book is sound, otherwise what is wrong.
    std::string validate() const;

private:
    // Both sides are keyed so that begin() is the best level:
    // asks by +price, bids by -price. One map type serves both sides.
    using LevelMap = std::map<int64_t, PriceLevel>;

    struct StopOrder {
        OrderId     id;
        Side        side;
        OrderType   type;
        TimeInForce tif;
        Price       limit_price;
        Quantity    qty;
        TraderId    trader;
    };
    // begin() is the next stop to trigger: buy stops by +stop (lowest first),
    // sell stops by -stop (highest first). multimap keeps FIFO within a price.
    using StopMap = std::multimap<int64_t, StopOrder>;

    struct EvAccept       { OrderId id; };
    struct EvReject       { OrderId id; const char* reason; };
    struct EvFill         { OrderId id; Price price; Quantity qty; Quantity remaining; };
    struct EvCancel       { OrderId id; Quantity remaining; };
    struct EvModify       { OrderId id; Price price; Quantity qty; };
    struct EvModifyReject { OrderId id; const char* reason; };
    struct EvTrigger      { OrderId id; };
    struct EvDepth        { Side side; LevelInfo level; };
    using Event = std::variant<EvAccept, EvReject, EvFill, EvCancel, EvModify,
                               EvModifyReject, EvTrigger, EvDepth, Trade, BookTop>;

    static int64_t key(Side side, Price p) { return side == Side::Buy ? -p.raw : p.raw; }

    LevelMap&       same_side(Side s)       { return s == Side::Buy ? bids_ : asks_; }
    LevelMap&       opposite(Side s)        { return s == Side::Buy ? asks_ : bids_; }
    const LevelMap& opposite(Side s) const  { return s == Side::Buy ? asks_ : bids_; }

    static bool crosses(const Order& taker, Price level_price);
    bool is_self_trade(const Order& taker, const Order& maker) const;
    bool can_fill_fully(const Order& taker) const;

    Order* make_order(OrderId id, Side side, OrderType type, Price price,
                      Quantity qty, Timestamp ts, TimeInForce tif, TraderId trader);
    void run(Order* order);            // match, then rest / cancel / release
    void trigger_stops(Timestamp ts);
    bool match(Order* taker);          // false => taker must be canceled (STP)
    void execute(Order* taker, Order* maker, PriceLevel& level, Quantity qty);
    void rest(Order* order);
    void unlink(Order* order);
    void release(Order* order);

    template <typename E>
    void emit(E&& e) {
        if (listener_) events_.emplace_back(std::forward<E>(e));
    }
    void mark_dirty(Side side, Price price);
    void finish();
    void dispatch(const Event& ev);

    std::string                         symbol_;
    LevelMap                            bids_;
    LevelMap                            asks_;
    std::unordered_map<OrderId, Order*> index_;
    MemoryPool<Order>&                  pool_;
    OrderListener*                      listener_;
    StpPolicy                           stp_;

    StopMap                                           buy_stops_;
    StopMap                                           sell_stops_;
    std::unordered_map<OrderId, StopMap::iterator>    stop_index_;
    std::optional<Price>                              last_trade_;

    std::vector<Event>                   events_;
    std::vector<std::pair<Side, Price>>  dirty_levels_;
    bool                                 dispatching_ = false;
    bool                                 depth_feed_  = false;
    BookTop                              last_top_;
};

} // namespace exchange
