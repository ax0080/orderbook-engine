#pragma once

#include "constants.h"
#include "types.h"

namespace exchange {

struct PriceLevel;

// Intrusive list node: resting orders link to each other directly, so
// joining or leaving a price level never allocates.
struct alignas(CACHE_LINE_SIZE) Order {
    OrderId     id              = 0;
    Price       price;
    Timestamp   timestamp       = 0;
    Order*      prev            = nullptr;
    Order*      next            = nullptr;
    PriceLevel* level           = nullptr;
    Quantity    quantity        = 0;
    Quantity    filled_quantity = 0;
    TraderId    trader          = 0;
    Side        side            = Side::Buy;
    OrderType   type            = OrderType::Limit;
    TimeInForce tif             = TimeInForce::GTC;
    OrderStatus status          = OrderStatus::New;

    Quantity remaining() const { return quantity - filled_quantity; }
};

static_assert(sizeof(Order) == CACHE_LINE_SIZE, "Order must fit in one cache line");

} // namespace exchange
