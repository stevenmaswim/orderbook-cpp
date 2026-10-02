#pragma once

// The reference model: a deliberately slow, obviously correct order book used
// only by tests. Every resting order sits in one flat vector. Every question
// ("what is the best maker?") is answered by scanning the whole vector, so
// each operation is O(n). It shares no data structures with the real books,
// so a bug in a map, list, index or ladder cannot hide in both. It implements
// DESIGN.md section 5 straight from the text.
//
// It has the same public interface as the real books, so the unit tests and
// the differential fuzz can drive it the same way.
#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

#include "ob/event.hpp"
#include "ob/snapshot.hpp"
#include "ob/types.hpp"
#include "ob/validate.hpp"

namespace obtest {

using namespace ob;

class ReferenceBook {
public:
    explicit ReferenceBook(BookConfig cfg = {}) : cfg_(cfg) {}

    template <EventSink S>
    void submit(const NewOrder& o, S& sink) {
        if (Reason r = validate(o, cfg_); r != Reason::None) {
            sink(Event::rejected(o.id, r));
            return;
        }
        if (find(o.id) != nullptr) {
            sink(Event::rejected(o.id, Reason::DuplicateId));
            return;
        }
        sink(Event::accepted(o));
        const bool has_limit = o.type != OrderType::Market;
        if (o.type == OrderType::FOK && available(o.side, o.price) < o.qty) {
            sink(Event::cancelled(o.id, o.side, o.qty, Reason::FokUnfillable));
            return;
        }
        const Qty left = match(o.id, o.side, has_limit, o.price, o.qty, sink);
        if (left == 0) return;
        if (o.type == OrderType::Limit) {
            orders_.push_back({o.id, o.side, o.price, left, next_seq_++});
        } else if (o.type == OrderType::Market) {
            sink(Event::cancelled(o.id, o.side, left, Reason::MarketRemainder));
        } else {
            sink(Event::cancelled(o.id, o.side, left, Reason::IocRemainder));
        }
    }

    template <EventSink S>
    void cancel(OrderId id, S& sink) {
        Ref* r = find(id);
        if (r == nullptr) {
            sink(Event::rejected(id, Reason::UnknownOrder));
            return;
        }
        sink(Event::cancelled(id, r->side, r->open, Reason::UserCancel));
        erase(id);
    }

    template <EventSink S>
    void modify(OrderId id, Price new_price, Qty new_qty, S& sink) {
        Ref* r = find(id);
        if (r == nullptr) {
            sink(Event::rejected(id, Reason::UnknownOrder));
            return;
        }
        if (Reason why = validate_modify(new_price, new_qty, cfg_); why != Reason::None) {
            sink(Event::rejected(id, why));
            return;
        }
        if (new_price == r->price && new_qty <= r->open) {
            r->open = new_qty;
            sink(Event::modified(id, r->side, new_price, new_qty, Reason::KeptPriority));
            return;
        }
        const Side side = r->side;
        erase(id);
        sink(Event::modified(id, side, new_price, new_qty, Reason::LostPriority));
        const Qty left = match(id, side, true, new_price, new_qty, sink);
        if (left > 0) orders_.push_back({id, side, new_price, left, next_seq_++});
    }

    std::optional<Price> best_bid() const { return best_price(Side::Buy); }
    std::optional<Price> best_ask() const { return best_price(Side::Sell); }
    std::size_t resting_count() const { return orders_.size(); }

    // Levels best first, orders oldest first, totals recounted from scratch.
    BookSnapshot snapshot() const {
        std::vector<Ref> sorted = orders_;
        std::sort(sorted.begin(), sorted.end(), [](const Ref& a, const Ref& b) {
            if (a.side != b.side) return a.side < b.side;
            if (a.price != b.price) return a.side == Side::Buy ? a.price > b.price : a.price < b.price;
            return a.seq < b.seq;
        });
        BookSnapshot s;
        for (const Ref& r : sorted) {
            std::vector<LevelView>& levels = r.side == Side::Buy ? s.bids : s.asks;
            if (levels.empty() || levels.back().price != r.price) levels.push_back({r.price, 0, {}});
            levels.back().total += r.open;
            levels.back().orders.push_back({r.id, r.open, r.seq});
        }
        return s;
    }

private:
    struct Ref {
        OrderId id;
        Side side;
        Price price;
        Qty open;
        Seq seq;
    };

    static bool crosses(Side taker, bool has_limit, Price limit, Price maker_price) {
        if (!has_limit) return true;
        return taker == Side::Buy ? maker_price <= limit : maker_price >= limit;
    }

    // Is maker a better than b? Better price first, then earlier arrival.
    static bool better(const Ref& a, const Ref& b) {
        if (a.price != b.price) return a.side == Side::Buy ? a.price > b.price : a.price < b.price;
        return a.seq < b.seq;
    }

    // Best maker for a taker on `side`, or nullptr if nothing crosses.
    Ref* best_maker(Side side, bool has_limit, Price limit) {
        Ref* best = nullptr;
        for (Ref& r : orders_) {
            if (r.side == side || !crosses(side, has_limit, limit, r.price)) continue;
            if (best == nullptr || better(r, *best)) best = &r;
        }
        return best;
    }

    template <EventSink S>
    Qty match(OrderId taker, Side side, bool has_limit, Price limit, Qty open, S& sink) {
        while (open > 0) {
            Ref* maker = best_maker(side, has_limit, limit);
            if (maker == nullptr) break;
            const Qty fill = std::min(open, maker->open);
            sink(Event::trade(taker, side, maker->id, maker->price, fill));
            open -= fill;
            maker->open -= fill;
            if (maker->open == 0) erase(maker->id);
        }
        return open;
    }

    Qty available(Side side, Price limit) const {
        Qty sum = 0;
        for (const Ref& r : orders_) {
            if (r.side != side && crosses(side, true, limit, r.price)) sum += r.open;
        }
        return sum;
    }

    std::optional<Price> best_price(Side side) const {
        std::optional<Price> best;
        for (const Ref& r : orders_) {
            if (r.side != side) continue;
            if (!best || (side == Side::Buy ? r.price > *best : r.price < *best)) best = r.price;
        }
        return best;
    }

    Ref* find(OrderId id) {
        for (Ref& r : orders_) {
            if (r.id == id) return &r;
        }
        return nullptr;
    }

    void erase(OrderId id) {
        orders_.erase(std::remove_if(orders_.begin(), orders_.end(),
                                     [id](const Ref& r) { return r.id == id; }),
                      orders_.end());
    }

    BookConfig cfg_;
    std::vector<Ref> orders_;
    Seq next_seq_ = 0;
};

}  // namespace obtest
