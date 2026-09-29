#pragma once

#include "order_book.h"
#include "types.h"

#include <type_traits>

namespace exchange {

// Fixed-size, trivially copyable request for passing orders between threads
// (e.g. gateway -> matching thread through an SpscQueue).
struct OrderCommand {
    enum class Kind : uint8_t { Submit, Cancel, Modify };

    OrderId     id    = 0;
    Price       price;
    Timestamp   ts    = 0;
    Quantity    qty   = 0;
    TraderId    trader = 0;
    Kind        kind  = Kind::Submit;
    Side        side  = Side::Buy;
    OrderType   type  = OrderType::Limit;
    TimeInForce tif   = TimeInForce::GTC;
};

static_assert(std::is_trivially_copyable_v<OrderCommand>);
static_assert(sizeof(OrderCommand) <= 48);

inline bool apply(OrderBook& book, const OrderCommand& c) {
    switch (c.kind) {
        case OrderCommand::Kind::Submit:
            return book.submit(c.id, c.side, c.type, c.price, c.qty, c.ts, c.tif, c.trader);
        case OrderCommand::Kind::Cancel:
            return book.cancel(c.id);
        case OrderCommand::Kind::Modify:
            return book.modify(c.id, c.price, c.qty, c.ts);
    }
    return false;
}

} // namespace exchange
