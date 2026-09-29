#pragma once

#include "types.h"

#include <string_view>

namespace exchange {

struct Trade {
    std::string_view symbol;   // points into the OrderBook; valid for the book's lifetime
    OrderId          buyer_order_id;
    OrderId          seller_order_id;
    Side             aggressor_side;
    Price            price;
    Quantity         quantity;
    Timestamp        timestamp;
};

// Top of book. A quantity of 0 means that side is empty.
struct BookTop {
    Price    bid_price;
    uint64_t bid_qty = 0;
    Price    ask_price;
    uint64_t ask_qty = 0;

    bool operator==(const BookTop& o) const {
        return bid_price == o.bid_price && bid_qty == o.bid_qty &&
               ask_price == o.ask_price && ask_qty == o.ask_qty;
    }
    bool operator!=(const BookTop& o) const { return !(*this == o); }
};

// One aggregated price level. quantity == 0 means the level was removed.
struct LevelInfo {
    Price    price;
    uint64_t quantity    = 0;
    uint32_t order_count = 0;
};

// Callbacks are delivered after the triggering operation has fully updated
// the book, so listeners may safely call back into the book/engine.
class OrderListener {
public:
    virtual ~OrderListener() = default;

    virtual void on_accept(OrderId) {}
    virtual void on_reject(OrderId, const char* /*reason*/) {}
    virtual void on_fill(OrderId, Price, Quantity /*qty*/, Quantity /*remaining*/) {}
    virtual void on_cancel(OrderId, Quantity /*remaining*/) {}
    virtual void on_modify(OrderId, Price, Quantity /*new_qty*/) {}
    virtual void on_modify_reject(OrderId, const char* /*reason*/) {}
    virtual void on_stop_trigger(OrderId) {}
    virtual void on_trade(const Trade&) {}
    virtual void on_bbo(std::string_view /*symbol*/, const BookTop&) {}
    // Incremental L2: one call per price level changed by an operation.
    virtual void on_depth(std::string_view /*symbol*/, Side, const LevelInfo&) {}
};

} // namespace exchange
