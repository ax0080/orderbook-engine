#include "exchange/matching_engine.h"

#include <chrono>

namespace exchange {

MatchingEngine::MatchingEngine(StpPolicy stp) : stp_(stp) {}

Timestamp MatchingEngine::now() {
    return static_cast<Timestamp>(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

OrderId MatchingEngine::submit_order(const std::string& symbol, Side side,
                                     OrderType type, Price price, Quantity qty,
                                     TimeInForce tif, TraderId trader) {
    OrderId    id   = next_id_++;
    OrderBook& book = get_or_create(symbol);

    // Registered up front; terminal callbacks (fill-to-zero, cancel, reject)
    // remove it again before submit() returns.
    routes_.emplace(id, &book);
    return book.submit(id, side, type, price, qty, now(), tif, trader) ? id : 0;
}

OrderId MatchingEngine::submit_stop_order(const std::string& symbol, Side side,
                                          OrderType type, Price stop_price,
                                          Price limit_price, Quantity qty,
                                          TimeInForce tif, TraderId trader) {
    OrderId    id   = next_id_++;
    OrderBook& book = get_or_create(symbol);

    routes_.emplace(id, &book);
    return book.submit_stop(id, side, type, stop_price, limit_price, qty, tif, trader) ? id : 0;
}

bool MatchingEngine::cancel_order(OrderId id) {
    auto it = routes_.find(id);
    return it != routes_.end() && it->second->cancel(id);
}

bool MatchingEngine::modify_order(OrderId id, Price new_price, Quantity new_qty) {
    auto it = routes_.find(id);
    if (it == routes_.end()) {
        if (listener_) listener_->on_modify_reject(id, "unknown order");
        return false;
    }
    return it->second->modify(id, new_price, new_qty, now());
}

OrderBook* MatchingEngine::get_book(const std::string& symbol) {
    auto it = books_.find(symbol);
    return it != books_.end() ? it->second.get() : nullptr;
}

const OrderBook* MatchingEngine::get_book(const std::string& symbol) const {
    auto it = books_.find(symbol);
    return it != books_.end() ? it->second.get() : nullptr;
}

OrderBook& MatchingEngine::get_or_create(const std::string& symbol) {
    auto it = books_.find(symbol);
    if (it != books_.end()) return *it->second;

    OrderListener* self = this;
    auto [ins, ok] = books_.emplace(
        symbol, std::make_unique<OrderBook>(symbol, pool_, self, stp_));
    ins->second->set_depth_feed(depth_feed_);
    return *ins->second;
}

void MatchingEngine::set_depth_feed(bool enabled) {
    depth_feed_ = enabled;
    for (auto& [sym, book] : books_) book->set_depth_feed(enabled);
}

// ---------------------------------------------------------------------------
// Book events: maintain routes_, then forward to the user's listener
// ---------------------------------------------------------------------------

void MatchingEngine::on_accept(OrderId id) {
    if (listener_) listener_->on_accept(id);
}

void MatchingEngine::on_reject(OrderId id, const char* reason) {
    routes_.erase(id);
    if (listener_) listener_->on_reject(id, reason);
}

void MatchingEngine::on_fill(OrderId id, Price p, Quantity q, Quantity remaining) {
    if (remaining == 0) routes_.erase(id);
    if (listener_) listener_->on_fill(id, p, q, remaining);
}

void MatchingEngine::on_cancel(OrderId id, Quantity remaining) {
    routes_.erase(id);
    if (listener_) listener_->on_cancel(id, remaining);
}

void MatchingEngine::on_modify(OrderId id, Price p, Quantity q) {
    if (listener_) listener_->on_modify(id, p, q);
}

void MatchingEngine::on_modify_reject(OrderId id, const char* reason) {
    if (listener_) listener_->on_modify_reject(id, reason);
}

void MatchingEngine::on_stop_trigger(OrderId id) {
    if (listener_) listener_->on_stop_trigger(id);
}

void MatchingEngine::on_depth(std::string_view symbol, Side side, const LevelInfo& level) {
    if (listener_) listener_->on_depth(symbol, side, level);
}

void MatchingEngine::on_trade(const Trade& t) {
    if (listener_) listener_->on_trade(t);
}

void MatchingEngine::on_bbo(std::string_view symbol, const BookTop& top) {
    if (listener_) listener_->on_bbo(symbol, top);
}

} // namespace exchange
