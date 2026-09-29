#pragma once

#include <cmath>
#include <cstdint>

namespace exchange {

using OrderId   = uint64_t;
using Quantity  = uint32_t;
using Timestamp = uint64_t;
using TraderId  = uint32_t;   // 0 = anonymous, exempt from self-trade prevention

struct Price {
    static constexpr int64_t MULTIPLIER = 10000;

    int64_t raw = 0;

    constexpr Price() = default;
    constexpr explicit Price(int64_t raw_value) : raw(raw_value) {}

    static Price from_double(double d) {
        return Price(static_cast<int64_t>(std::llround(d * MULTIPLIER)));
    }

    constexpr double to_double() const {
        return static_cast<double>(raw) / MULTIPLIER;
    }

    constexpr bool operator==(Price o) const { return raw == o.raw; }
    constexpr bool operator!=(Price o) const { return raw != o.raw; }
    constexpr bool operator< (Price o) const { return raw <  o.raw; }
    constexpr bool operator> (Price o) const { return raw >  o.raw; }
    constexpr bool operator<=(Price o) const { return raw <= o.raw; }
    constexpr bool operator>=(Price o) const { return raw >= o.raw; }
};

enum class Side : uint8_t {
    Buy,
    Sell
};

enum class OrderType : uint8_t {
    Limit,
    Market
};

enum class TimeInForce : uint8_t {
    GTC,   // rest until canceled
    IOC,   // fill what is possible, cancel the rest
    FOK    // fill entirely or cancel entirely
};

enum class OrderStatus : uint8_t {
    New,
    PartiallyFilled,
    Filled,
    Canceled,
    Rejected
};

enum class StpPolicy : uint8_t {
    None,           // allow self-trades
    CancelTaker,    // cancel the incoming order
    CancelResting,  // cancel the resting order, keep matching
    CancelBoth
};

} // namespace exchange
