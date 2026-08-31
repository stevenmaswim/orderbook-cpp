#include "orderbook/order_book.hpp"

#include <algorithm>
#include <map>

namespace orderbook {

void OrderBook::push_handle(const Order& o) {
    if (o.side == Side::Buy) {
        bids_.push(Handle{o.price, o.sequence, o.id});
    } else {
        asks_.push(Handle{o.price, o.sequence, o.id});
    }
}

Order* OrderBook::peek_top_live(Side side) {
    if (side == Side::Buy) {
        while (!bids_.empty()) {
            const Handle& h = bids_.top();
            auto it = orders_.find(h.id);
            if (it != orders_.end() && it->second.is_live()) return &it->second;
            bids_.pop();  // lazy deletion of a dead / stale handle
        }
    } else {
        while (!asks_.empty()) {
            const Handle& h = asks_.top();
            auto it = orders_.find(h.id);
            if (it != orders_.end() && it->second.is_live()) return &it->second;
            asks_.pop();
        }
    }
    return nullptr;
}

Order* OrderBook::pop_top_live(Side side) {
    Order* top = peek_top_live(side);
    if (top == nullptr) return nullptr;
    if (side == Side::Buy) {
        bids_.pop();
    } else {
        asks_.pop();
    }
    return top;
}

Trade OrderBook::execute(Order& buy, Order& sell, std::int64_t price) {
    std::int64_t qty = std::min(buy.remaining(), sell.remaining());
    buy.apply_fill(qty);
    sell.apply_fill(qty);
    Trade t{buy.id, sell.id, price, qty, next_sequence_++};
    return t;
}

std::vector<Trade> OrderBook::add_order(const std::string& id, Side side,
                                        std::int64_t price,
                                        std::int64_t quantity) {
    std::vector<Trade> made;
    if (quantity <= 0) return made;
    auto existing = orders_.find(id);
    if (existing != orders_.end() && existing->second.is_live()) {
        return made;  // duplicate live id -> reject
    }

    Order incoming;
    incoming.id = id;
    incoming.price = price;
    incoming.quantity = quantity;
    incoming.side = side;
    incoming.sequence = next_sequence_++;
    orders_[id] = incoming;
    Order& order = orders_[id];

    // Match against the opposite side while marketable.
    if (side == Side::Buy) {
        while (order.remaining() > 0) {
            Order* best = peek_top_live(Side::Sell);
            if (best == nullptr || best->price > order.price) break;
            Order* sell = pop_top_live(Side::Sell);
            Trade t = execute(order, *sell, sell->price);
            made.push_back(t);
            if (sell->state == OrderState::PartiallyFilled) push_handle(*sell);
        }
    } else {
        while (order.remaining() > 0) {
            Order* best = peek_top_live(Side::Buy);
            if (best == nullptr || best->price < order.price) break;
            Order* buy = pop_top_live(Side::Buy);
            Trade t = execute(*buy, order, buy->price);
            made.push_back(t);
            if (buy->state == OrderState::PartiallyFilled) push_handle(*buy);
        }
    }

    // Rest any unfilled remainder on the book.
    if (order.remaining() > 0) push_handle(order);

    trades_.insert(trades_.end(), made.begin(), made.end());
    return made;
}

bool OrderBook::cancel_order(const std::string& id) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return false;
    if (!it->second.is_live()) return false;
    it->second.state = OrderState::Canceled;
    return true;  // handle is left in the heap; skipped lazily later
}

std::optional<std::int64_t> OrderBook::best_bid() {
    Order* top = peek_top_live(Side::Buy);
    if (top == nullptr) return std::nullopt;
    return top->price;
}

std::optional<std::int64_t> OrderBook::best_ask() {
    Order* top = peek_top_live(Side::Sell);
    if (top == nullptr) return std::nullopt;
    return top->price;
}

std::optional<std::int64_t> OrderBook::spread() {
    auto b = best_bid();
    auto a = best_ask();
    if (!b || !a) return std::nullopt;
    return *a - *b;
}

OrderBook::Depth OrderBook::depth(std::size_t levels) {
    // Aggregate live remaining volume by price from the authoritative map so
    // stale heap handles never distort the picture.
    std::map<std::int64_t, std::int64_t, std::greater<>> bid_vol;  // desc
    std::map<std::int64_t, std::int64_t> ask_vol;                  // asc
    for (const auto& [id, o] : orders_) {
        if (!o.is_live()) continue;
        if (o.side == Side::Buy) {
            bid_vol[o.price] += o.remaining();
        } else {
            ask_vol[o.price] += o.remaining();
        }
    }
    Depth d;
    for (const auto& [p, v] : bid_vol) {
        if (d.bids.size() >= levels) break;
        d.bids.push_back({p, v});
    }
    for (const auto& [p, v] : ask_vol) {
        if (d.asks.size() >= levels) break;
        d.asks.push_back({p, v});
    }
    return d;
}

std::size_t OrderBook::live_order_count() const {
    std::size_t n = 0;
    for (const auto& [id, o] : orders_) {
        if (o.is_live()) ++n;
    }
    return n;
}

}  // namespace orderbook
