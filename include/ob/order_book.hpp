#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <vector>

#include "ob/assert.hpp"
#include "ob/event.hpp"
#include "ob/level.hpp"
#include "ob/snapshot.hpp"
#include "ob/types.hpp"
#include "ob/validate.hpp"

namespace ob {

// The upgraded book. Same contract as BaselineBook (DESIGN.md section 5), with
// intrusive FIFO levels (upgrade A), and the two remaining design choices
// pulled out as template policies so each one can be measured on its own in
// the same benchmark run:
//   Ladder  : how price levels are found (MapLadder, or ArrayLadder from C)
//   Storage : where Order nodes live and how ids find them (HeapStorage, or
//             PoolStorage from B)
//
// Single-threaded by design (DESIGN.md section 8). Not thread-safe.
template <class Ladder, class Storage>
class OrderBook {
public:
    explicit OrderBook(BookConfig cfg = {})
        : cfg_(cfg), bids_(Side::Buy, cfg), asks_(Side::Sell, cfg), store_(cfg) {}

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    template <EventSink S>
    void submit(const NewOrder& o, S& sink) {
        if (Reason r = validate(o, cfg_); r != Reason::None) {
            sink(Event::rejected(o.id, r));
            return;
        }
        if (store_.find(o.id) != nullptr) {
            sink(Event::rejected(o.id, Reason::DuplicateId));
            return;
        }
        // A limit order might need a node to rest in. Refuse it up front if
        // storage is full, before any event says it was accepted. (Only a
        // fixed-size pool can be full; this is conservative: it also refuses a
        // limit order that would have filled completely.)
        if (o.type == OrderType::Limit && store_.full()) {
            sink(Event::rejected(o.id, Reason::CapacityExceeded));
            return;
        }
        sink(Event::accepted(o));

        if (o.type == OrderType::FOK && !fok_fillable(o.side, o.price, o.qty)) {
            sink(Event::cancelled(o.id, o.side, o.qty, Reason::FokUnfillable));
            return;
        }

        const Qty open = match(o.id, o.side, effective_limit(o), o.qty, sink);
        if (open == 0) return;

        switch (o.type) {
            case OrderType::Limit:
                rest(o.id, o.side, o.price, open);
                break;
            case OrderType::Market:
                sink(Event::cancelled(o.id, o.side, open, Reason::MarketRemainder));
                break;
            case OrderType::IOC:
                sink(Event::cancelled(o.id, o.side, open, Reason::IocRemainder));
                break;
            case OrderType::FOK:
                OB_ASSERT(false && "FOK passed its pre-check but did not fill");
                break;
        }
    }

    template <EventSink S>
    void cancel(OrderId id, S& sink) {
        Order* o = store_.find(id);
        if (o == nullptr) {
            sink(Event::rejected(id, Reason::UnknownOrder));
            return;
        }
        sink(Event::cancelled(id, o->side, o->open, Reason::UserCancel));
        remove(o);
    }

    template <EventSink S>
    void modify(OrderId id, Price new_price, Qty new_qty, S& sink) {
        Order* o = store_.find(id);
        if (o == nullptr) {
            sink(Event::rejected(id, Reason::UnknownOrder));
            return;
        }
        if (Reason r = validate_modify(new_price, new_qty, cfg_); r != Reason::None) {
            sink(Event::rejected(id, r));
            return;
        }
        if (new_price == o->price && new_qty <= o->open) {
            o->level->total -= o->open - new_qty;  // keep the level total exact
            o->open = new_qty;
            sink(Event::modified(id, o->side, new_price, new_qty, Reason::KeptPriority));
            return;
        }
        // Cancel-replace. remove() frees a node first, so the re-rest below
        // can never find storage full.
        const Side side = o->side;
        remove(o);
        sink(Event::modified(id, side, new_price, new_qty, Reason::LostPriority));
        const Qty open = match(id, side, new_price, new_qty, sink);
        if (open > 0) rest(id, side, new_price, open);
    }

    std::optional<Price> best_bid() const { return best_of(bids_); }
    std::optional<Price> best_ask() const { return best_of(asks_); }
    std::size_t resting_count() const { return store_.size(); }

    BookSnapshot snapshot() const {
        BookSnapshot s;
        copy_side(bids_, s.bids);
        copy_side(asks_, s.asks);
        return s;
    }

private:
    Ladder& ladder(Side s) { return s == Side::Buy ? bids_ : asks_; }
    const Ladder& ladder(Side s) const { return s == Side::Buy ? bids_ : asks_; }

    static std::optional<Price> best_of(const Ladder& l) {
        const Level* b = l.best();
        return b ? std::optional<Price>{b->price} : std::nullopt;
    }

    static bool crosses(Side side, Price limit, Price px) {
        return side == Side::Buy ? px <= limit : px >= limit;
    }

    // Market = limit at the worst possible price, so one loop serves all types.
    static Price effective_limit(const NewOrder& o) {
        if (o.type != OrderType::Market) return o.price;
        return o.side == Side::Buy ? std::numeric_limits<Price>::max()
                                   : std::numeric_limits<Price>::min();
    }

    bool fok_fillable(Side side, Price limit, Qty qty) const {
        Qty available = 0;
        ladder(opposite(side)).for_each_from_best([&](const Level& level) {
            if (!crosses(side, limit, level.price)) return false;
            available += level.total;
            return available < qty;  // keep walking only while still short
        });
        return available >= qty;
    }

    template <EventSink S>
    Qty match(OrderId taker, Side side, Price limit, Qty open, S& sink) {
        Ladder& book = ladder(opposite(side));
        while (open > 0) {
            Level* level = book.best();
            if (level == nullptr || !crosses(side, limit, level->price)) break;
            while (open > 0 && !level->empty()) {
                Order* maker = level->head;
                const Qty fill = std::min(open, maker->open);
                sink(Event::trade(taker, side, maker->id, maker->price, fill));
                open -= fill;
                maker->open -= fill;
                level->total -= fill;
                if (maker->open == 0) {
                    level->unlink(maker);
                    store_.erase(maker->id);
                    store_.release(maker);
                }
            }
            if (level->empty()) book.erase_empty(level);
        }
        return open;
    }

    void rest(OrderId id, Side side, Price price, Qty open) {
        OB_ASSERT(open > 0);
        Order* o = store_.acquire();
        OB_ASSERT(o != nullptr);
        o->id = id;
        o->price = price;
        o->open = open;
        o->seq = next_seq_++;
        o->side = side;
        ladder(side).get_or_create(price)->push_back(o);
        store_.insert(id, o);
    }

    void remove(Order* o) {
        Level* level = o->level;
        level->unlink(o);
        if (level->empty()) ladder(o->side).erase_empty(level);
        store_.erase(o->id);
        store_.release(o);
    }

    static void copy_side(const Ladder& l, std::vector<LevelView>& out) {
        l.for_each_from_best([&](const Level& level) {
            LevelView v{level.price, level.total, {}};
            for (const Order* o = level.head; o != nullptr; o = o->next) {
                v.orders.push_back({o->id, o->open, o->seq});
            }
            out.push_back(std::move(v));
            return true;
        });
    }

    BookConfig cfg_;
    Ladder bids_;
    Ladder asks_;
    Storage store_;
    Seq next_seq_ = 0;
};

}  // namespace ob
