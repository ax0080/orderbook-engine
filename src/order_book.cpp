#include "exchange/order_book.h"

#include <algorithm>
#include <type_traits>

namespace exchange {

// ---------------------------------------------------------------------------
// PriceLevel (intrusive FIFO)
// ---------------------------------------------------------------------------

void PriceLevel::push_back(Order* o) {
    o->prev  = tail;
    o->next  = nullptr;
    o->level = this;
    if (tail) tail->next = o;
    else      head = o;
    tail = o;
    total_qty += o->remaining();
    ++count;
}

void PriceLevel::erase(Order* o) {
    if (o->prev) o->prev->next = o->next;
    else         head = o->next;
    if (o->next) o->next->prev = o->prev;
    else         tail = o->prev;
    total_qty -= o->remaining();
    --count;
    o->prev = o->next = nullptr;
    o->level = nullptr;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

OrderBook::OrderBook(std::string symbol, MemoryPool<Order>& pool,
                     OrderListener* listener, StpPolicy stp)
    : symbol_(std::move(symbol)), pool_(pool), listener_(listener), stp_(stp) {
    events_.reserve(64);
    dirty_levels_.reserve(16);
}

OrderBook::~OrderBook() {
    for (LevelMap* side : {&bids_, &asks_}) {
        for (auto& [k, level] : *side) {
            for (Order* o = level.head; o;) {
                Order* next = o->next;
                pool_.deallocate(o);
                o = next;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Public operations
// ---------------------------------------------------------------------------

bool OrderBook::submit(OrderId id, Side side, OrderType type, Price price,
                       Quantity qty, Timestamp ts, TimeInForce tif,
                       TraderId trader) {
    const char* reason = nullptr;
    if (qty == 0)                                        reason = "zero quantity";
    else if (type == OrderType::Limit && price.raw <= 0) reason = "invalid price for limit order";
    else if (index_.count(id) || stop_index_.count(id))  reason = "duplicate order id";
    if (reason) {
        emit(EvReject{id, reason});
        finish();
        return false;
    }

    emit(EvAccept{id});
    run(make_order(id, side, type, price, qty, ts, tif, trader));
    trigger_stops(ts);
    finish();
    return true;
}

bool OrderBook::submit_stop(OrderId id, Side side, OrderType type,
                            Price stop_price, Price limit_price, Quantity qty,
                            TimeInForce tif, TraderId trader) {
    const bool  buy    = side == Side::Buy;
    const char* reason = nullptr;
    if (qty == 0)                                              reason = "zero quantity";
    else if (stop_price.raw <= 0)                              reason = "invalid stop price";
    else if (type == OrderType::Limit && limit_price.raw <= 0) reason = "invalid price for limit order";
    else if (index_.count(id) || stop_index_.count(id))        reason = "duplicate order id";
    else if (last_trade_ && (buy ? *last_trade_ >= stop_price
                                 : *last_trade_ <= stop_price))
        reason = "stop would trigger immediately";
    if (reason) {
        emit(EvReject{id, reason});
        finish();
        return false;
    }

    StopMap& stops = buy ? buy_stops_ : sell_stops_;
    auto it = stops.emplace(buy ? stop_price.raw : -stop_price.raw,
                            StopOrder{id, side, type, tif, limit_price, qty, trader});
    stop_index_.emplace(id, it);

    emit(EvAccept{id});
    finish();
    return true;
}

bool OrderBook::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) {
        auto sit = stop_index_.find(id);
        if (sit == stop_index_.end()) return false;

        StopMap::iterator stop = sit->second;
        emit(EvCancel{id, stop->second.qty});
        (stop->second.side == Side::Buy ? buy_stops_ : sell_stops_).erase(stop);
        stop_index_.erase(sit);
        finish();
        return true;
    }

    Order* order = it->second;
    index_.erase(it);
    emit(EvCancel{id, order->remaining()});
    unlink(order);
    order->status = OrderStatus::Canceled;
    pool_.deallocate(order);

    finish();
    return true;
}

bool OrderBook::modify(OrderId id, Price new_price, Quantity new_qty,
                       Timestamp ts) {
    auto it = index_.find(id);
    const char* reason = nullptr;
    if (it == index_.end())
        reason = stop_index_.count(id) ? "stop orders cannot be modified" : "unknown order";
    else if (new_price.raw <= 0)                     reason = "invalid price";
    else if (new_qty <= it->second->filled_quantity) reason = "quantity at or below filled";
    if (reason) {
        emit(EvModifyReject{id, reason});
        finish();
        return false;
    }

    Order* order = it->second;
    emit(EvModify{id, new_price, new_qty});

    if (new_price == order->price && new_qty <= order->quantity) {
        order->level->total_qty -= order->quantity - new_qty;
        order->quantity = new_qty;
        mark_dirty(order->side, order->price);
        finish();
        return true;
    }

    unlink(order);
    index_.erase(it);
    order->price     = new_price;
    order->quantity  = new_qty;
    order->timestamp = ts;
    run(order);
    trigger_stops(ts);

    finish();
    return true;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

const Order* OrderBook::find(OrderId id) const {
    auto it = index_.find(id);
    return it != index_.end() ? it->second : nullptr;
}

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->second.price;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->second.price;
}

BookTop OrderBook::top() const {
    BookTop t;
    if (!bids_.empty()) {
        t.bid_price = bids_.begin()->second.price;
        t.bid_qty   = bids_.begin()->second.total_qty;
    }
    if (!asks_.empty()) {
        t.ask_price = asks_.begin()->second.price;
        t.ask_qty   = asks_.begin()->second.total_qty;
    }
    return t;
}

std::vector<LevelInfo> OrderBook::depth(Side side, std::size_t max_levels) const {
    const LevelMap& levels = (side == Side::Buy) ? bids_ : asks_;
    std::vector<LevelInfo> out;
    out.reserve(std::min(max_levels, levels.size()));
    for (const auto& [k, level] : levels) {
        if (out.size() == max_levels) break;
        out.push_back({level.price, level.total_qty, level.count});
    }
    return out;
}

std::string OrderBook::validate() const {
    std::size_t resting = 0;
    for (Side side : {Side::Buy, Side::Sell}) {
        const LevelMap& levels = side == Side::Buy ? bids_ : asks_;
        for (const auto& [k, level] : levels) {
            if (k != key(side, level.price))            return "level key does not match its price";
            if (level.empty())                          return "empty level left in map";
            if (level.head->prev || level.tail->next)   return "list ends not terminated";

            uint64_t     qty  = 0;
            uint32_t     n    = 0;
            const Order* prev = nullptr;
            for (const Order* o = level.head; o; prev = o, o = o->next) {
                if (o->prev != prev)                    return "broken prev link";
                if (o->level != &level)                 return "order points to wrong level";
                if (o->side != side || o->price != level.price) return "order on wrong level";
                if (o->remaining() == 0)                return "filled order still resting";
                auto it = index_.find(o->id);
                if (it == index_.end() || it->second != o) return "resting order missing from index";
                qty += o->remaining();
                ++n;
            }
            if (prev != level.tail)                     return "tail does not match last order";
            if (qty != level.total_qty || n != level.count) return "level totals out of sync";
            resting += n;
        }
    }
    if (resting != index_.size())                       return "index holds orders not on the book";
    if (!bids_.empty() && !asks_.empty() && *best_bid() >= *best_ask())
        return "book is crossed";
    if (stop_index_.size() != buy_stops_.size() + sell_stops_.size())
        return "stop index out of sync";
    return {};
}

// ---------------------------------------------------------------------------
// Order execution
// ---------------------------------------------------------------------------

Order* OrderBook::make_order(OrderId id, Side side, OrderType type, Price price,
                             Quantity qty, Timestamp ts, TimeInForce tif,
                             TraderId trader) {
    Order* o     = pool_.allocate();
    o->id        = id;
    o->side      = side;
    o->type      = type;
    o->tif       = tif;
    o->price     = price;
    o->quantity  = qty;
    o->timestamp = ts;
    o->trader    = trader;
    return o;
}

// The order must not be in index_ or on a level when this is called.
void OrderBook::run(Order* order) {
    bool alive = false;
    if (order->tif != TimeInForce::FOK || can_fill_fully(*order))
        alive = match(order);

    if (order->remaining() == 0) {
        order->status = OrderStatus::Filled;
        pool_.deallocate(order);
    } else if (!alive || order->type == OrderType::Market ||
               order->tif != TimeInForce::GTC) {
        order->status = OrderStatus::Canceled;
        emit(EvCancel{order->id, order->remaining()});
        pool_.deallocate(order);
    } else {
        index_.emplace(order->id, order);
        rest(order);
    }
}

// Iterative, not recursive: each triggered stop may trade and move the last
// price, which can trigger more stops. The loop runs until nothing fires.
void OrderBook::trigger_stops(Timestamp ts) {
    while (last_trade_) {
        const int64_t last = last_trade_->raw;
        StopMap* stops = nullptr;
        if (!buy_stops_.empty() && last >= buy_stops_.begin()->first)
            stops = &buy_stops_;
        else if (!sell_stops_.empty() && last <= -sell_stops_.begin()->first)
            stops = &sell_stops_;
        if (!stops) break;

        StopOrder s = stops->begin()->second;
        stops->erase(stops->begin());
        stop_index_.erase(s.id);

        emit(EvTrigger{s.id});
        run(make_order(s.id, s.side, s.type, s.limit_price, s.qty, ts, s.tif, s.trader));
    }
}

// ---------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------

bool OrderBook::crosses(const Order& taker, Price level_price) {
    if (taker.type == OrderType::Market) return true;
    return taker.side == Side::Buy ? level_price <= taker.price
                                   : level_price >= taker.price;
}

bool OrderBook::is_self_trade(const Order& taker, const Order& maker) const {
    return stp_ != StpPolicy::None && taker.trader != 0 &&
           taker.trader == maker.trader;
}

bool OrderBook::can_fill_fully(const Order& taker) const {
    const uint64_t need = taker.remaining();
    uint64_t available  = 0;
    const bool check_stp = stp_ != StpPolicy::None && taker.trader != 0;

    for (const auto& [k, level] : opposite(taker.side)) {
        if (!crosses(taker, level.price)) break;
        if (!check_stp) {
            available += level.total_qty;
        } else {
            for (const Order* o = level.head; o; o = o->next) {
                if (is_self_trade(taker, *o)) {
                    if (stp_ == StpPolicy::CancelResting) continue;
                    return false;   // taker would be canceled here
                }
                available += o->remaining();
                if (available >= need) return true;
            }
        }
        if (available >= need) return true;
    }
    return false;
}

bool OrderBook::match(Order* taker) {
    LevelMap& book = opposite(taker->side);

    while (taker->remaining() > 0 && !book.empty()) {
        auto        level_it = book.begin();
        PriceLevel& level    = level_it->second;
        if (!crosses(*taker, level.price)) break;

        while (taker->remaining() > 0 && !level.empty()) {
            Order* maker = level.head;

            if (is_self_trade(*taker, *maker)) {
                if (stp_ == StpPolicy::CancelResting || stp_ == StpPolicy::CancelBoth) {
                    emit(EvCancel{maker->id, maker->remaining()});
                    mark_dirty(maker->side, level.price);
                    level.erase(maker);
                    release(maker);
                }
                if (stp_ == StpPolicy::CancelTaker || stp_ == StpPolicy::CancelBoth) {
                    if (level.empty()) book.erase(level_it);
                    return false;
                }
                continue;
            }

            execute(taker, maker, level, std::min(taker->remaining(), maker->remaining()));

            if (maker->remaining() == 0) {
                maker->status = OrderStatus::Filled;
                level.erase(maker);
                release(maker);
            }
        }
        if (level.empty()) book.erase(level_it);
    }
    return true;
}

void OrderBook::execute(Order* taker, Order* maker, PriceLevel& level,
                        Quantity qty) {
    taker->filled_quantity += qty;
    maker->filled_quantity += qty;
    level.total_qty        -= qty;
    maker->status = OrderStatus::PartiallyFilled;

    const Price price = maker->price;
    const bool  buy   = taker->side == Side::Buy;
    last_trade_ = price;
    mark_dirty(maker->side, price);

    emit(Trade{symbol_, buy ? taker->id : maker->id, buy ? maker->id : taker->id,
               taker->side, price, qty, taker->timestamp});
    emit(EvFill{taker->id, price, qty, taker->remaining()});
    emit(EvFill{maker->id, price, qty, maker->remaining()});
}

void OrderBook::rest(Order* order) {
    order->status = order->filled_quantity > 0 ? OrderStatus::PartiallyFilled
                                               : OrderStatus::New;
    auto [it, inserted] = same_side(order->side).try_emplace(key(order->side, order->price));
    if (inserted) it->second.price = order->price;
    it->second.push_back(order);
    mark_dirty(order->side, order->price);
}

void OrderBook::unlink(Order* order) {
    PriceLevel* level = order->level;
    mark_dirty(order->side, level->price);
    level->erase(order);
    if (level->empty()) same_side(order->side).erase(key(order->side, level->price));
}

// Removes a resting order that was taken off its level during matching.
void OrderBook::release(Order* order) {
    index_.erase(order->id);
    pool_.deallocate(order);
}

// ---------------------------------------------------------------------------
// Event delivery
// ---------------------------------------------------------------------------

void OrderBook::mark_dirty(Side side, Price price) {
    if (!depth_feed_ || !listener_) return;
    if (!dirty_levels_.empty() && dirty_levels_.back().first == side &&
        dirty_levels_.back().second == price)
        return;
    dirty_levels_.emplace_back(side, price);
}

// Called at the end of every public operation. Events are queued during the
// operation and delivered only once the book is consistent. A listener that
// re-enters the book appends to the same queue, which the outermost call drains.
void OrderBook::finish() {
    if (!listener_) return;

    if (!dirty_levels_.empty()) {
        std::sort(dirty_levels_.begin(), dirty_levels_.end());
        dirty_levels_.erase(std::unique(dirty_levels_.begin(), dirty_levels_.end()),
                            dirty_levels_.end());
        for (auto [side, price] : dirty_levels_) {
            const LevelMap& levels = side == Side::Buy ? bids_ : asks_;
            auto it = levels.find(key(side, price));
            LevelInfo info{price, 0, 0};
            if (it != levels.end()) info = {price, it->second.total_qty, it->second.count};
            events_.emplace_back(EvDepth{side, info});
        }
        dirty_levels_.clear();
    }

    BookTop t = top();
    if (t != last_top_) {
        last_top_ = t;
        events_.emplace_back(t);
    }
    if (dispatching_) return;

    struct Reset {
        OrderBook& b;
        ~Reset() { b.events_.clear(); b.dispatching_ = false; }
    } reset{*this};
    dispatching_ = true;

    for (std::size_t i = 0; i < events_.size(); ++i) {
        Event ev = events_[i];   // copy: re-entrant calls may reallocate events_
        dispatch(ev);
    }
}

void OrderBook::dispatch(const Event& ev) {
    std::visit([this](const auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, EvAccept>)            listener_->on_accept(e.id);
        else if constexpr (std::is_same_v<T, EvReject>)       listener_->on_reject(e.id, e.reason);
        else if constexpr (std::is_same_v<T, EvFill>)         listener_->on_fill(e.id, e.price, e.qty, e.remaining);
        else if constexpr (std::is_same_v<T, EvCancel>)       listener_->on_cancel(e.id, e.remaining);
        else if constexpr (std::is_same_v<T, EvModify>)       listener_->on_modify(e.id, e.price, e.qty);
        else if constexpr (std::is_same_v<T, EvModifyReject>) listener_->on_modify_reject(e.id, e.reason);
        else if constexpr (std::is_same_v<T, EvTrigger>)      listener_->on_stop_trigger(e.id);
        else if constexpr (std::is_same_v<T, EvDepth>)        listener_->on_depth(symbol_, e.side, e.level);
        else if constexpr (std::is_same_v<T, Trade>)          listener_->on_trade(e);
        else                                                  listener_->on_bbo(symbol_, e);
    }, ev);
}

} // namespace exchange
