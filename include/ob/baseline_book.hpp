#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>

#include "ob/assert.hpp"
#include "ob/event.hpp"
#include "ob/snapshot.hpp"
#include "ob/types.hpp"
#include "ob/validate.hpp"

namespace ob {

// The v0 baseline: standard containers only, chosen to be easy to see as
// correct. It allocates on almost every operation on purpose. Upgrades A, B and
// C each remove one source of that cost and are measured against this class,
// which stays in the tree as the comparison point (and as one more book the
// differential fuzz test checks).
//
//   bids_/asks_ : std::map<Price, Level>, sorted best first, so best = begin()
//   Level       : std::list FIFO of resting orders + running total qty
//   index_      : order id -> iterators to its level and its list node
//
// Single-threaded by design (DESIGN.md section 8). Not thread-safe.
class BaselineBook {
public:
    explicit BaselineBook(BookConfig cfg = {})
        : cfg_(cfg), bids_(PriceOrder{true}), asks_(PriceOrder{false}) {}

    // Accept a new order: match what crosses, then rest the remainder (Limit)
    // or cancel it (Market, IOC). FOK either fills completely or not at all.
    // Events: Accepted, Trade*, then at most one Cancelled. Or a lone Rejected.
    template <EventSink S>
    void submit(const NewOrder& o, S& sink) {
        if (Reason r = validate(o, cfg_); r != Reason::None) {
            sink(Event::rejected(o.id, r));
            return;
        }
        // Only a live id is a duplicate. Remembering every id ever used would
        // need unbounded memory (DESIGN.md section 5).
        if (index_.contains(o.id)) {
            sink(Event::rejected(o.id, Reason::DuplicateId));
            return;
        }
        sink(Event::accepted(o));

        // FOK is all-or-nothing, so check the whole fill is possible before
        // touching anything. Uses level totals: O(levels crossed), not O(orders).
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
                // No price to rest at, so the unfilled part is cancelled.
                sink(Event::cancelled(o.id, o.side, open, Reason::MarketRemainder));
                break;
            case OrderType::IOC:
                sink(Event::cancelled(o.id, o.side, open, Reason::IocRemainder));
                break;
            case OrderType::FOK:
                // The pre-check said the full qty was available and nothing
                // can change between the check and the match (single thread).
                OB_ASSERT(false && "FOK passed its pre-check but did not fill");
                break;
        }
    }

    // Remove a resting order. O(1) average: hash lookup, then erase by the
    // stored iterators. Events: Cancelled(UserCancel), or Rejected(UnknownOrder)
    // if the id is not resting (never seen, filled, already cancelled, or an
    // order type that never rests).
    template <EventSink S>
    void cancel(OrderId id, S& sink) {
        auto it = index_.find(id);
        if (it == index_.end()) {
            sink(Event::rejected(id, Reason::UnknownOrder));
            return;
        }
        sink(Event::cancelled(id, it->second.side, it->second.order->open, Reason::UserCancel));
        remove(it);
    }

    // Change a resting order's price and/or open qty (DESIGN.md section 5).
    //  - same price, qty not larger: shrink in place, keep queue position
    //  - price change or qty increase: cancel-replace with the same id; it
    //    goes to the back of the queue and may trade if the new price crosses
    // Events: Modified then any Trades, or a lone Rejected. An unknown id is
    // checked before the new values, so it wins if both are wrong.
    template <EventSink S>
    void modify(OrderId id, Price new_price, Qty new_qty, S& sink) {
        auto it = index_.find(id);
        if (it == index_.end()) {
            sink(Event::rejected(id, Reason::UnknownOrder));
            return;
        }
        if (Reason r = validate_modify(new_price, new_qty, cfg_); r != Reason::None) {
            sink(Event::rejected(id, r));
            return;
        }
        const Locator loc = it->second;
        RestingOrder& order = *loc.order;
        if (new_price == order.price && new_qty <= order.open) {
            // Shrinking cannot hurt anyone behind this order in the queue, so
            // it keeps its place. The difference is cancelled qty.
            loc.level->second.total -= order.open - new_qty;
            order.open = new_qty;
            sink(Event::modified(id, loc.side, new_price, new_qty, Reason::KeptPriority));
            return;
        }
        // Growing in place would let a trader keep queue position while adding
        // size ahead of later arrivals, so an increase (or any price change)
        // re-enters at the back as a brand new arrival.
        const Side side = loc.side;
        remove(it);
        sink(Event::modified(id, side, new_price, new_qty, Reason::LostPriority));
        const Qty open = match(id, side, new_price, new_qty, sink);
        if (open > 0) rest(id, side, new_price, open);
    }

    std::optional<Price> best_bid() const {
        if (bids_.empty()) return std::nullopt;
        return bids_.begin()->first;
    }
    std::optional<Price> best_ask() const {
        if (asks_.empty()) return std::nullopt;
        return asks_.begin()->first;
    }

    std::size_t resting_count() const { return index_.size(); }

    BookSnapshot snapshot() const {
        BookSnapshot s;
        copy_side(bids_, s.bids);
        copy_side(asks_, s.asks);
        return s;
    }

private:
    struct RestingOrder {
        OrderId id;
        Price price;
        Qty open;
        Seq seq;
        Side side;
    };
    using Fifo = std::list<RestingOrder>;

    struct Level {
        Fifo fifo;
        Qty total = 0;  // sum of fifo[i].open, kept up to date on every change
    };

    // One comparator type for both sides, with the direction chosen at run
    // time. With std::greater for bids and std::less for asks the two maps
    // would be different types, so one Locator could not hold an iterator into
    // either. The branch is perfectly predictable within a map.
    struct PriceOrder {
        bool descending;
        bool operator()(Price a, Price b) const { return descending ? a > b : a < b; }
    };
    using LevelMap = std::map<Price, Level, PriceOrder>;

    // std::map and std::list iterators stay valid while other elements are
    // inserted or erased, so storing them lets cancel jump straight to the
    // node instead of searching (DESIGN.md section 6).
    struct Locator {
        LevelMap::iterator level;
        Fifo::iterator order;
        Side side;
    };

    LevelMap& side_map(Side s) { return s == Side::Buy ? bids_ : asks_; }

    // Does a taker on `side` with this limit trade against a level at `px`?
    static bool crosses(Side side, Price limit, Price px) {
        return side == Side::Buy ? px <= limit : px >= limit;
    }

    // A market order is treated as a limit order at the worst possible price,
    // so the one matching loop serves every order type.
    static Price effective_limit(const NewOrder& o) {
        if (o.type != OrderType::Market) return o.price;
        return o.side == Side::Buy ? std::numeric_limits<Price>::max()
                                   : std::numeric_limits<Price>::min();
    }

    // Is there at least `qty` on the opposite side at prices this order
    // would accept? Stops as soon as the answer is yes.
    bool fok_fillable(Side side, Price limit, Qty qty) const {
        const LevelMap& book = side == Side::Buy ? asks_ : bids_;
        Qty available = 0;
        for (const auto& [price, level] : book) {
            if (!crosses(side, limit, price)) break;
            available += level.total;
            if (available >= qty) return true;
        }
        return false;
    }

    // Fill `open` against the opposite side, best level first and FIFO within
    // a level. Returns what is left. Trades print at the maker's price.
    template <EventSink S>
    Qty match(OrderId taker, Side side, Price limit, Qty open, S& sink) {
        LevelMap& book = side_map(opposite(side));
        while (open > 0 && !book.empty()) {
            auto level_it = book.begin();  // best opposite price
            if (!crosses(side, limit, level_it->first)) break;
            Level& level = level_it->second;
            while (open > 0 && !level.fifo.empty()) {
                RestingOrder& maker = level.fifo.front();  // oldest at this price
                const Qty fill = std::min(open, maker.open);
                sink(Event::trade(taker, side, maker.id, maker.price, fill));
                open -= fill;
                maker.open -= fill;
                level.total -= fill;
                if (maker.open == 0) {
                    index_.erase(maker.id);
                    level.fifo.pop_front();
                }
            }
            // Never leave an empty level behind: best_bid()/best_ask() rely on
            // begin() being a level that really has orders.
            if (level.fifo.empty()) book.erase(level_it);
        }
        return open;
    }

    void rest(OrderId id, Side side, Price price, Qty open) {
        OB_ASSERT(open > 0);
        LevelMap& book = side_map(side);
        auto [level_it, created] = book.try_emplace(price);
        Level& level = level_it->second;
        level.fifo.push_back(RestingOrder{id, price, open, next_seq_++, side});
        level.total += open;
        index_.emplace(id, Locator{level_it, std::prev(level.fifo.end()), side});
    }

    // Unlink a resting order from its level, its side and the index.
    void remove(std::unordered_map<OrderId, Locator>::iterator index_it) {
        const Locator loc = index_it->second;
        Level& level = loc.level->second;
        level.total -= loc.order->open;
        level.fifo.erase(loc.order);
        if (level.fifo.empty()) side_map(loc.side).erase(loc.level);
        index_.erase(index_it);
    }

    static void copy_side(const LevelMap& book, std::vector<LevelView>& out) {
        for (const auto& [price, level] : book) {
            LevelView v{price, level.total, {}};
            for (const RestingOrder& r : level.fifo) v.orders.push_back({r.id, r.open, r.seq});
            out.push_back(std::move(v));
        }
    }

    BookConfig cfg_;
    LevelMap bids_;
    LevelMap asks_;
    std::unordered_map<OrderId, Locator> index_;
    Seq next_seq_ = 0;
};

}  // namespace ob
